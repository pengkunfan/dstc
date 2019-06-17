// Copyright (C) 2018, Jaguar Land Rover
// This program is licensed under the terms and conditions of the
// Mozilla Public License, version 2.0.  The full text of the
// Mozilla Public License is at https://www.mozilla.org/MPL/2.0/
//
// Author: Magnus Feuer (mfeuer1@jaguarlandrover.com)

// Server that can load and execute lambda functions.
// See README.md for details

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include "dstc.h"
#include <rmc_log.h>

#define MAX_CONNECTIONS 16
#define SUSPEND_TRAFFIC_THRESHOLD 3000
#define RESTART_TRAFFIC_THRESHOLD 2800

static int _dstc_initialized = 0;
dstc_context_t _dstc_default_context;
// All DSTC_CLIENT-registered functions (dstc_print_name_and_age)
// and their string name.
// These need to be global since they are setup by
// dstc_register_client_function() called by DSTC_CLIENT() macro
// as a part of generated __attribute__((constructor)) functions.

// FIXME: Hash table
static dstc_client_func_t _dstc_client_func[SYMTAB_SIZE] ;
static uint32_t _dstc_client_func_ind = 0;

static uint32_t _dstc_client_callback_count = 0;

// All local server functions that can be called by remote nodes
// FIXME: Hash table
static dstc_server_func_t _dstc_server_func[SYMTAB_SIZE];
static uint32_t _dstc_server_func_ind;

#define TO_EPOLL_EVENT_USER_DATA(_index, is_pub) (index | ((is_pub)?USER_DATA_PUB_FLAG:0) | DSTC_EVENT_FLAG)
#define FROM_EPOLL_EVENT_USER_DATA(_user_data) (_user_data & USER_DATA_INDEX_MASK & ~DSTC_EVENT_FLAG)
#define IS_PUB(_user_data) (((_user_data) & USER_DATA_PUB_FLAG)?1:0)


typedef struct {
    rmc_node_id_t node_id;
    char name[256];
} dstc_control_message_t;


char* _op_res_string(uint8_t res)
{
    switch(res) {
    case RMC_ERROR:
        return "error";

    case RMC_READ_MULTICAST:
        return "read multicast";

    case RMC_READ_MULTICAST_LOOPBACK:
        return "multicast loopback";

    case RMC_READ_MULTICAST_NEW:
        return "new multicast";

    case RMC_READ_MULTICAST_NOT_READY:
        return "multicast not ready";

    case RMC_READ_TCP:
        return "read tcp";

    case RMC_READ_ACCEPT:
        return "accept";

    case RMC_READ_DISCONNECT:
        return "disconnect";

    case RMC_WRITE_MULTICAST:
        return "write multicast";

    case RMC_COMPLETE_CONNECTION:
        return "complete connection";

    case RMC_WRITE_TCP:
        return "tcp write";

    default:
        return "[unknown]";

    }
}

static uint32_t dstc_payload_buffer_in_use(dstc_context_t* ctx)
{
    return ctx->pub_buffer_ind;
}

static uint32_t dstc_payload_buffer_available(dstc_context_t* ctx)
{
    return sizeof(ctx->pub_buffer) - ctx->pub_buffer_ind;
}

static uint8_t* dstc_payload_buffer(dstc_context_t* ctx)
{
    return ctx->pub_buffer;
}

static uint8_t* dstc_payload_buffer_alloc(dstc_context_t* ctx, uint32_t size)
{
    uint8_t* res = 0;
    if (dstc_payload_buffer_available(ctx) < size)
        return 0;

    res = dstc_payload_buffer(ctx) + dstc_payload_buffer_in_use(ctx);
    ctx->pub_buffer_ind += size;
    return res;
}


static uint8_t* dstc_payload_buffer_empty(dstc_context_t* ctx)
{
    ctx->pub_buffer_ind = 0;
    return 0;
}

// Retrieve a function pointer by name previously registered with
// dstc_register_server_function()
//
static dstc_internal_dispatch_t dstc_find_server_function(char* name)
{
    int i = _dstc_server_func_ind;
    while(i--) {
        if (!strcmp(_dstc_server_func[i].func_name, name))
            return _dstc_server_func[i].server_func;
    }
    return (dstc_internal_dispatch_t) 0;
}


static int queue_pending_calls(void)
{
    // If we have pending data, and we are not suspended, queue the
    // payload with reliable multicast.
    if (rmc_pub_traffic_suspended(_dstc_default_context.pub_ctx) == 0 &&
        // Do we have data that we need to queue?
        dstc_payload_buffer_in_use(&_dstc_default_context) > 0) {
        uint8_t* rmc_data = malloc(dstc_payload_buffer_in_use(&_dstc_default_context));

        if (!rmc_data) {
            RMC_LOG_FATAL("malloc(%d): %s", dstc_payload_buffer_in_use(&_dstc_default_context), strerror(errno));
            exit(255);
        }

        memcpy(rmc_data, dstc_payload_buffer(&_dstc_default_context), dstc_payload_buffer_in_use(&_dstc_default_context));
        // This should never fail since we are not suspended.
        if (rmc_pub_queue_packet(_dstc_default_context.pub_ctx,
                                 rmc_data,
                                 dstc_payload_buffer_in_use(&_dstc_default_context),
                                 0) != 0) {
            RMC_LOG_FATAL("Failed to queue packet.");
            exit(255);
        }

        // Was the queueing successful?
        RMC_LOG_DEBUG("Queued %d bytes from payload buffer.", dstc_payload_buffer_in_use(&_dstc_default_context));
        // Empty payload buffer.
        dstc_payload_buffer_empty(&_dstc_default_context);
    }
    return 0;
}

// Register a function name - pointer relationship.
// Called by file constructor function _dstc_register_[name]()
// generated by DSTC_SERVER() macro.
//
void dstc_register_server_function(char* name, dstc_internal_dispatch_t server_func)
{
    int ind = 0;

    ind = _dstc_server_func_ind;
    if (ind == SYMTAB_SIZE - 1) {
        RMC_LOG_FATAL("Out of memory trying to register server function. SYMTAB_SIZE=%d\n", SYMTAB_SIZE);
        exit(255);
    }

    strcpy(_dstc_server_func[ind].func_name, name);
    _dstc_server_func[ind].server_func = server_func;
    _dstc_server_func_ind++;
}

void dstc_register_client_function(char* name, void *client_func)
{
    int ind = 0;

    ind = _dstc_client_func_ind;
    if (ind == SYMTAB_SIZE - 1) {
        RMC_LOG_FATAL("Out of memory trying to register client function. SYMTAB_SIZE=%d\n", SYMTAB_SIZE);
        exit(255);
    }

    strcpy(_dstc_client_func[ind].func_name, name);
    _dstc_client_func[ind].client_func = client_func;
    _dstc_client_func_ind++;
}


// Retrieve a callback function. Each time it is invoked, it will be deleted.
// dstc_register_server_function()
//
static dstc_internal_dispatch_t dstc_find_callback_by_func(dstc_internal_dispatch_t func)
{
    int i = 0;
    while(i < _dstc_default_context.callback_ind) {
        if (_dstc_default_context.local_callback[i].callback == func) {
            dstc_internal_dispatch_t res = _dstc_default_context.local_callback[i].callback;
            // Nil out the callback since it is a one-time shot thing.
            _dstc_default_context.local_callback[i].callback = 0;
            _dstc_default_context.local_callback[i].callback_ref = 0;
            return res;
        }
        ++i;
    }
    RMC_LOG_COMMENT("Did not find callback [%p]\n", func);
    return (dstc_internal_dispatch_t) 0;
}

static dstc_internal_dispatch_t dstc_find_callback_by_ref(dstc_callback_t callback_ref)
{
    int i = 0;
    while(i < _dstc_default_context.callback_ind) {
        if (_dstc_default_context.local_callback[i].callback_ref == callback_ref) {
            dstc_internal_dispatch_t res = _dstc_default_context.local_callback[i].callback;
            // Nil out the callback since it is a one-time shot thing.
            _dstc_default_context.local_callback[i].callback = 0;
            _dstc_default_context.local_callback[i].callback_ref = 0;
            return res;
        }
        ++i;
    }
    RMC_LOG_COMMENT("Did not find callback [%lX]", callback_ref);
    return (dstc_internal_dispatch_t) 0;
}


// Activate a client-side callback that can be invoked from a remote
// DSTC function called from the client.  Called by the
// CLIENT_CALLBACK_ARG() macro to register a relationship between a
// callback reference integer and a pointer to the dispatch function
// that handles the incoming callback from the remote DSTC function.
// callback_ref is a pointer to the callback function, but can be any
// unique uint64_t. This integer is passed as a reference to the
// remote DSTC function, which will send it back to the client
// in order to invoke the local callback.
//
// client-side dstc_process_function_call() will detect that
// a callback is being invoked and will use dstc_find_callback_by_ref()
// to map the provided reference callback integer to a dispatch function,
// which is then called.
// dstc_find_callback_by_ref() will also de-activate the callback,
// stopping it from being invoked multiple time.
//
dstc_callback_t dstc_activate_callback(dstc_callback_t callback_ref,
                                       dstc_internal_dispatch_t callback)
{
    int ind = 0;
    // Find a previously freed slot, or allocate a new one
    while(ind < _dstc_default_context.callback_ind) {
        if (!_dstc_default_context.local_callback[ind].callback)
            break;
        ++ind;
    }

    // Are we out of memory
    if (ind == SYMTAB_SIZE) {
        RMC_LOG_FATAL("Out of memory trying to register callback. SYMTAB_SIZE=%d\n", SYMTAB_SIZE);
        exit(255);
    }
    _dstc_default_context.local_callback[ind].callback_ref = callback_ref;
    _dstc_default_context.local_callback[ind].callback = callback;
    RMC_LOG_COMMENT("Registered server callback [%llX] to %p. Index[%d]",
                    callback_ref, callback, ind);

    // If we are allocating a new slot (not reusing an earlier one).
    // then bump callback_ind to the new max index in use.
    if (ind == _dstc_default_context.callback_ind)
        _dstc_default_context.callback_ind++;

    return callback_ref;
}

// Register a callback function name - pointer relationship.
// Called by file constructor function _dstc_register_callback_[name]()
// generated by DSTC_CALLBACK() macro.
// For now, we just bump a counter to figure out if we should
// send out announce messages or not.
//
void dstc_register_callback_client(char* name, void* callback)
{
  _dstc_client_callback_count++;
}


void dstc_cancel_callback(dstc_internal_dispatch_t callback)
{
    // Will delete the callback.
    dstc_find_callback_by_func( callback);
}

uint8_t dstc_remote_function_available_by_name(char* func_name)
{
    int ind = 0;

    // Scan all remotely registered nodes and their functions
    // to see if you can find one with a matching na,e
    ind = _dstc_default_context.remote_node_ind;
    while(ind--) {
        if (!strcmp(func_name, _dstc_default_context.remote_node[ind].func_name))
            return 1;
    }
    RMC_LOG_DEBUG("Could not find a remote node that had registered function %s", func_name);
    return 0;
}

uint8_t dstc_remote_function_available(void* client_func)
{
    int ind = 0;

    // Find the string name for the dstc_[func_name] function
    // pointer provided in client_func

    ind = _dstc_client_func_ind;
    while(ind--)
        if (_dstc_client_func[ind].client_func == client_func)
            break;

    // No hit?
    if (ind == -1) {
        return 0;
    }

    return dstc_remote_function_available_by_name(_dstc_client_func[ind].func_name);
}

// Register a remote function as provided by the remote DSTC server
// through a control message call processed by
// dstc_subscriber_control_message_cb()
//
static void dstc_register_remote_function(rmc_node_id_t node_id, char* func_name)
{
    int ind = 0;
    dstc_remote_node_t* remote = 0;

    // See if the node has registered any prior functions
    // If so, check that we don't have a duplicate and then register
    // the new function.
    ind = _dstc_default_context.remote_node_ind;
    while(ind--) {
        if (node_id == _dstc_default_context.remote_node[ind].node_id &&
            !strcmp(func_name, _dstc_default_context.remote_node[ind].func_name)) {
            RMC_LOG_WARNING("Remote function [%s] registered several times by node [0x%X]",
                            func_name, node_id);
            return;
        }
    }

    if (_dstc_default_context.remote_node_ind == SYMTAB_SIZE) {
        RMC_LOG_FATAL("Out of memory trying to register remote func. SYMTAB_SIZE=%d\n", SYMTAB_SIZE);
        exit(255);
    }

    remote = &_dstc_default_context.remote_node[_dstc_default_context.remote_node_ind];
    remote->node_id = node_id;
    strcpy(remote->func_name, func_name);

    _dstc_default_context.remote_node_ind++;
    RMC_LOG_INFO("Remote [%s] now supported by new node [0x%X]",
                 func_name, node_id);
    return;
}


// Remove all functions previously registered by node_id through
// the dstc_register_remote_function() call.
static void dstc_unregister_remote_node(rmc_node_id_t node_id)
{
    int ind = _dstc_default_context.remote_node_ind;

    while(ind--) {
        if (node_id == _dstc_default_context.remote_node[ind].node_id) {
            RMC_LOG_INFO("Uhregistering node [0x%X] function [%s]",
                         _dstc_default_context.remote_node[ind].node_id,
                         _dstc_default_context.remote_node[ind].func_name);

            _dstc_default_context.remote_node[ind].node_id = 0;
            _dstc_default_context.remote_node[ind].func_name[0] = 0;
        }
    }
}



static void poll_add(user_data_t user_data,
                     int descriptor,
                     uint32_t event_user_data,
                     rmc_poll_action_t action)
{
    struct epoll_event ev = {
        .data.u32 = event_user_data,
        .events = 0 // EPOLLONESHOT
    };

    if (action & RMC_POLLREAD)
        ev.events |= EPOLLIN;

    if (action & RMC_POLLWRITE)
        ev.events |= EPOLLOUT;

    if (epoll_ctl(_dstc_default_context.epoll_fd, EPOLL_CTL_ADD, descriptor, &ev) == -1) {
        RMC_LOG_INDEX_FATAL(FROM_EPOLL_EVENT_USER_DATA(event_user_data), "epoll_ctl(add) event_udata[%lX]",
                            event_user_data);
        exit(255);
    }
}


static void poll_add_sub(user_data_t user_data,
                         int descriptor,
                         rmc_index_t index,
                         rmc_poll_action_t action)
{
    poll_add(user_data, descriptor, TO_EPOLL_EVENT_USER_DATA(index, 0), action);
}

static void poll_add_pub(user_data_t user_data,
                         int descriptor,
                         rmc_index_t index,
                         rmc_poll_action_t action)
{
    poll_add(user_data, descriptor, TO_EPOLL_EVENT_USER_DATA(index, 1), action);
}

static void poll_modify(user_data_t user_data,
                        int descriptor,
                        uint32_t event_user_data,
                        rmc_poll_action_t old_action,
                        rmc_poll_action_t new_action)
{
    struct epoll_event ev = {
        .data.u32 = event_user_data,
        .events = 0 // EPOLLONESHOT
    };

    if (old_action == new_action)
        return ;

    if (new_action & RMC_POLLREAD)
        ev.events |= EPOLLIN;

    if (new_action & RMC_POLLWRITE)
        ev.events |= EPOLLOUT;

    if (epoll_ctl(_dstc_default_context.epoll_fd, EPOLL_CTL_MOD, descriptor, &ev) == -1) {
        RMC_LOG_INDEX_FATAL(FROM_EPOLL_EVENT_USER_DATA(event_user_data), "epoll_ctl(modify): %s", strerror(errno));
        exit(255);
    }
}

static void poll_modify_pub(user_data_t user_data,
                            int descriptor,
                            rmc_index_t index,
                            rmc_poll_action_t old_action,
                            rmc_poll_action_t new_action)
{
    poll_modify(user_data,
                descriptor,
                TO_EPOLL_EVENT_USER_DATA(index, 1),
                old_action,
                new_action);
}

static void poll_modify_sub(user_data_t user_data,
                            int descriptor,
                            rmc_index_t index,
                            rmc_poll_action_t old_action,
                            rmc_poll_action_t new_action)
{
    poll_modify(user_data,
                descriptor,
                TO_EPOLL_EVENT_USER_DATA(index, 0),
                old_action,
                new_action);
}


static void poll_remove(user_data_t user_data,
                        int descriptor,
                        rmc_index_t index)
{
    if (epoll_ctl(_dstc_default_context.epoll_fd, EPOLL_CTL_DEL, descriptor, 0) == -1) {
        RMC_LOG_INDEX_WARNING(index, "epoll_ctl(delete): %s", strerror(errno));
        return;
    }
    RMC_LOG_INDEX_COMMENT(index, "poll_remove() desc[%d] index[%d]", descriptor, index);
}


usec_timestamp_t dstc_get_timeout_timestamp()
{
    usec_timestamp_t sub_event_tout_ts = 0;
    usec_timestamp_t pub_event_tout_ts = 0;

    rmc_pub_timeout_get_next(_dstc_default_context.pub_ctx, &pub_event_tout_ts);
    rmc_sub_timeout_get_next(_dstc_default_context.sub_ctx, &sub_event_tout_ts);

    // Figure out the shortest event timeout between pub and sub context
    if (pub_event_tout_ts == -1 && sub_event_tout_ts == -1)
        return -1;

    if (pub_event_tout_ts == -1 && sub_event_tout_ts != -1)
        return sub_event_tout_ts;

    if (pub_event_tout_ts != -1 && sub_event_tout_ts == -1)
        return pub_event_tout_ts;

    return (pub_event_tout_ts < sub_event_tout_ts)?
        pub_event_tout_ts:sub_event_tout_ts;
}


int dstc_get_timeout_msec(void)
{
    usec_timestamp_t tout = dstc_get_timeout_timestamp();

    if (tout == -1)
        return -1;

    // Convert to relative timestamp.
    tout -= rmc_usec_monotonic_timestamp();
    if (tout < 0)
        return 0;

    return tout / 1000 + 1;
}


int dstc_process_single_event(int timeout)
{
    int nfds = 0;
    int retval = 0;
    if (!_dstc_initialized)
        dstc_setup();

    struct epoll_event events[dstc_get_socket_count()];

    nfds = epoll_wait(_dstc_default_context.epoll_fd, events, sizeof(events) / sizeof(events[0]), timeout);

    if (nfds == -1) {
        RMC_LOG_FATAL("epoll_wait(%d): %s", _dstc_default_context.epoll_fd, strerror(errno));
        exit(255);
    }

    // Timeout
    if (nfds == 0) {
        if (!dstc_get_timeout_msec())
            dstc_process_timeout();

        retval = ETIME;
    }

    // Process all pending events.
    while(nfds--)
        dstc_process_epoll_result(&events[nfds]);

    if (!dstc_get_timeout_msec())
        dstc_process_timeout();

    return retval;
}

int dstc_process_events(usec_timestamp_t timeout_arg)
{
    usec_timestamp_t timeout_arg_ts = 0;
    usec_timestamp_t now = 0;

    if (!_dstc_initialized)
        dstc_setup();


    // Is this a one-pass thing where we just want to process all pending
    // epoll events and timeout and then return?
    if (!timeout_arg)
        return dstc_process_single_event(0);

    // Calculate an absolute timeout timestamp based on relative
    // timestamp provided in argument.

    timeout_arg_ts = (timeout_arg == -1)?-1:(rmc_usec_monotonic_timestamp() + timeout_arg);

    // Process evdents until we reach the timeout therhold.
    while((now = rmc_usec_monotonic_timestamp()) < timeout_arg_ts || timeout_arg_ts == -1) {
        usec_timestamp_t timeout = 0;
        char is_arg_timeout = 0;
        usec_timestamp_t event_tout_rel = 0;
        usec_timestamp_t timeout_arg_rel = (timeout_arg_ts == -1)?-1:(timeout_arg_ts - now) / 1000 + 1 ;

        event_tout_rel = dstc_get_timeout_msec();

        // Figure out the shortest timeout between argument and event timeout
        if (timeout_arg_rel == -1 && event_tout_rel == -1) {
            RMC_LOG_DEBUG("Both argument and event timeout are -1 -> -1");
            timeout = -1;
        }

        if (timeout_arg_rel == -1 && event_tout_rel != -1) {
            timeout = event_tout_rel; // Will never be less than 0
            RMC_LOG_DEBUG("arg timeout == -1. Event timeout != -1 -> %ld", timeout);
        }

        if (timeout_arg_rel != -1 && event_tout_rel == -1) {
            is_arg_timeout = 1;
            timeout = timeout_arg_rel; // Will never be less than 0
            RMC_LOG_DEBUG("arg timeout != -1. Event timeout == -1 -> %ld", timeout);
        }

        if (timeout_arg_rel != -1 && event_tout_rel != -1) {
            if (event_tout_rel < timeout_arg_rel) {
                timeout = event_tout_rel;
                RMC_LOG_DEBUG("event timeout is less than arg timeout -> %ld", timeout);
            } else {
                timeout = timeout_arg_rel;
                RMC_LOG_DEBUG("arg timeout is less than event timeout -> %ld", timeout);
                is_arg_timeout = 1;
            }
        }

        if (dstc_process_single_event((int) timeout) == ETIME) {
            // Did we time out on an RMC event to be processed, or did
            // we time out on the argument provided to
            // dstc_process_events()?
            if (is_arg_timeout) {
                RMC_LOG_DEBUG("Timed out on argument. returning" );
                return ETIME;
            }

            continue;
        }
    }

    return 0;
}

extern void dstc_process_epoll_result(struct epoll_event* event)
{
    uint8_t op_res = 0;
    rmc_index_t c_ind = (rmc_index_t) FROM_EPOLL_EVENT_USER_DATA(event->data.u32);
    int is_pub = IS_PUB(event->data.u32);

    RMC_LOG_INDEX_DEBUG(c_ind, "%s: %s%s%s",
                        (is_pub?"pub":"sub"),
                        ((event->events & EPOLLIN)?" read":""),
                        ((event->events & EPOLLOUT)?" write":""),
                        ((event->events & EPOLLHUP)?" disconnect":""));


    if (event->events & EPOLLIN) {
        if (is_pub)
            rmc_pub_read(_dstc_default_context.pub_ctx, c_ind, &op_res);
        else
            rmc_sub_read(_dstc_default_context.sub_ctx, c_ind, &op_res);
    }

    if (event->events & EPOLLOUT) {
        if (is_pub) {
            if (rmc_pub_write(_dstc_default_context.pub_ctx, c_ind, &op_res) != 0)
                rmc_pub_close_connection(_dstc_default_context.pub_ctx, c_ind);
        } else {
            if (rmc_sub_write(_dstc_default_context.sub_ctx, c_ind, &op_res) != 0)
                rmc_sub_close_connection(_dstc_default_context.sub_ctx, c_ind);
        }
    }
}

extern int dstc_process_timeout(void)
{
    // If either of the timeout processor fails in with EAGAIN, then they
    // tried resending un-acknolwedged packets but encountered full transmissions
    // queues in rmc.
    // In that case process events until the queues are sent out on the network
    // and are cleared up.
    if (rmc_pub_timeout_process(_dstc_default_context.pub_ctx) == EAGAIN ||
        rmc_sub_timeout_process(_dstc_default_context.sub_ctx) == EAGAIN)
        return EAGAIN;

    return 0;
}

static uint32_t dstc_process_function_call(uint8_t* data,
                                           uint32_t data_len)
{
    dstc_header_t* call = (dstc_header_t*) data;
    dstc_internal_dispatch_t local_func_ptr = 0;
    dstc_callback_t callback_ref = 0;

    if (data_len < sizeof(dstc_header_t)) {
        RMC_LOG_WARNING("Packet header too short! Wanted %ld bytes, got %d",
                        sizeof(dstc_header_t), data_len);
        return data_len; // Emtpy buffer
    }

    if (data_len - sizeof(dstc_header_t) < call->payload_len) {
        RMC_LOG_WARNING("Packet payload too short! Wanted %d bytes, got %d",
                        call->payload_len, data_len - sizeof(dstc_header_t));
        return data_len; // Emtpy buffer
    }

    // Retrieve function pointer from name, as previously
    // registered with dstc_register_server_function()
    RMC_LOG_DEBUG("DSTC Serve: node_id[%lu] name[%s] payload_len[%d]",
                  call->node_id,
                  call->payload,
                  call->payload_len - strlen((char*) call->payload) - 1);
    // If the name is not nil-len, then we have an actual server function we need
    // to find and invoke.
    if (call->payload[0]) {
        size_t name_len = strlen((char*) call->payload);
        local_func_ptr = dstc_find_server_function((char*) call->payload);

        if (!local_func_ptr) {
            RMC_LOG_COMMENT("Function [%s] not loaded. Ignored", call->payload);
            return sizeof(dstc_header_t) + call->payload_len;
        }
        RMC_LOG_DEBUG("Making local function call node_id[%u] func_name[%s] payload_len[%u]",
                      call->node_id,
                      call->payload,
                      call->payload_len - name_len - 1);
        (*local_func_ptr)(0, // Callback ref is 0
                          call->node_id,
                          call->payload, // function name
                          call->payload + name_len + 1, // Payload
                          call->payload_len - name_len - 1);  // Payload len

        return sizeof(dstc_header_t) + call->payload_len;
    }

    // If name is nil-len, then the eight bytes after the initial \0 is
    // the callback reference value
    callback_ref = *((dstc_callback_t*)(call->payload + 1));
    local_func_ptr = dstc_find_callback_by_ref(callback_ref);

    if (!local_func_ptr) {
        RMC_LOG_COMMENT("Callback [%llu] not loaded. Ignored", (long long unsigned) callback_ref);
        return sizeof(dstc_header_t) + call->payload_len;
    }
    (*local_func_ptr)(callback_ref,
                      call->node_id,
                      call->payload, // Funcation name. Always ""
                      call->payload + 1 + sizeof(uint64_t),// Payload after nil name and uint64_t
                      call->payload_len - 1 - sizeof(uint64_t));  // Payload len

    return sizeof(dstc_header_t) + call->payload_len;
}


static void dstc_subscription_complete(rmc_sub_context_t* sub_ctx,
                                       uint32_t listen_ip,
                                       in_port_t listen_port,
                                       rmc_node_id_t node_id)
{
    int ind = _dstc_server_func_ind;
    RMC_LOG_COMMENT("Subscription complete. Sending supported functions.");

    // Retrieve function pointer from name, as previously
    // registered with dstc_register_dstc_default_context.local_function()
    // Include null terminator for an easier life.
    while(ind--) {
        RMC_LOG_COMMENT("  [%s]", _dstc_server_func[ind].func_name);
        dstc_control_message_t ctl = {
            .node_id = node_id
        };

        strcpy(ctl.name, _dstc_server_func[ind].func_name);

        rmc_sub_write_control_message_by_node_id(sub_ctx,
                                                 node_id,
                                                 &ctl,
                                                 sizeof(rmc_node_id_t) +
                                                 sizeof(uint8_t) +
                                                 strlen(ctl.name) + 1);

    }
    RMC_LOG_COMMENT("Done sending functions");
    return;
}

static void dstc_process_incoming(rmc_sub_context_t* sub_ctx)
{
    rmc_sub_packet_t* pack = 0;
    RMC_LOG_DEBUG("Processing incoming");
    while((pack = rmc_sub_get_next_dispatch_ready(sub_ctx))) {
        uint32_t ind = 0;

        RMC_LOG_DEBUG("Got packet. payload_len[%d]", rmc_sub_packet_payload_len(pack));
        while(ind < rmc_sub_packet_payload_len(pack)) {
            RMC_LOG_DEBUG("Processing function call. ind[%d]", ind);
            ind += dstc_process_function_call(((uint8_t*) rmc_sub_packet_payload(pack) + ind),
                                               rmc_sub_packet_payload_len(pack) - ind);
        }

        rmc_sub_packet_dispatched(sub_ctx, pack);
    }
    return;
}


static void dstc_subscriber_control_message_cb(rmc_pub_context_t* ctx,
                                               uint32_t publisher_address,
                                               uint16_t publisher_port,
                                               rmc_node_id_t node_id,
                                               void* payload,
                                               payload_len_t payload_len)
{
    dstc_control_message_t *ctl = (dstc_control_message_t*) payload;

    dstc_register_remote_function(ctl->node_id, ctl->name);
    return;
}

static void dstc_subscriber_disconnect_cb(rmc_pub_context_t* ctx,
                                          uint32_t publisher_address,
                                          uint16_t publisher_port)
{
    dstc_unregister_remote_node(rmc_pub_node_id(ctx));
    return;
}


uint32_t dstc_get_socket_count(void)
{
    if (!_dstc_initialized)
        return 0;

    // Grab the count of all open sockets.
    return rmc_sub_get_socket_count(_dstc_default_context.sub_ctx) +
        rmc_pub_get_socket_count(_dstc_default_context.pub_ctx);
}


static void free_published_packets(void* pl, payload_len_t len, user_data_t dt)
{
    RMC_LOG_DEBUG("Freeing %p", pl);
    free(pl);
}


static int dstc_setup_internal(dstc_context_t* ctx,
                               rmc_node_id_t node_id,
                               int max_dstc_nodes,
                               char* multicast_group_addr,
                               int multicast_port,
                               char* multicast_iface_addr,
                               int mcast_ttl,
                               char* control_listen_iface_addr,
                               int control_listen_port,
                               int epoll_fd_arg,
                               user_data_t user_data)
{
    if (!ctx || epoll_fd_arg == -1)
        return EINVAL;

    ctx->epoll_fd = epoll_fd_arg;
    ctx->remote_node_ind = 0;
    ctx->callback_ind = 0;
    ctx->pub_buffer_ind = 0;
    ctx->pub_ctx = 0;
    ctx->sub_ctx = 0;


    rmc_log_set_start_time();
    rmc_pub_init_context(&ctx->pub_ctx,
                         node_id, // Node ID
                         multicast_group_addr, multicast_port,
                         multicast_iface_addr,  // Use any NIC address for multicast transmit.
                         control_listen_iface_addr, // Use any NIC address for listen control port.
                         control_listen_port, // Use ephereal tcp port for tcp control
                         user_data,
                         poll_add_pub, poll_modify_pub, poll_remove,
                         MAX_CONNECTIONS,
                         free_published_packets);

    // Setup a callback for subscriber disconnect, meaning that remote nodes
    // with functions that we can call can no longer be used.
    rmc_pub_set_subscriber_disconnect_callback(ctx->pub_ctx,
                                               dstc_subscriber_disconnect_cb);

    // Setup a subscriber callback, allowing us to know when a subscribe that can
    // execute the function has attached.
    rmc_pub_set_control_message_callback(ctx->pub_ctx, dstc_subscriber_control_message_cb);

    rmc_pub_throttling(ctx->pub_ctx,
                       SUSPEND_TRAFFIC_THRESHOLD,
                       RESTART_TRAFFIC_THRESHOLD);

    // Subscriber init.
    rmc_sub_init_context(&ctx->sub_ctx,
                         // Reuse pub node id to detect and avoid loopback messages
                         rmc_pub_node_id(ctx->pub_ctx),
                         multicast_group_addr, multicast_port,
                         multicast_iface_addr,  // Use any NIC address for multicast transmit.
                         user_data,
                         poll_add_sub, poll_modify_sub, poll_remove,
                         MAX_CONNECTIONS,
                         0,0);

    rmc_sub_set_packet_ready_callback(ctx->sub_ctx, dstc_process_incoming);
    rmc_sub_set_subscription_complete_callback(ctx->sub_ctx, dstc_subscription_complete);

    rmc_pub_set_multicast_ttl(ctx->pub_ctx, mcast_ttl);
    rmc_pub_activate_context(ctx->pub_ctx);
    rmc_sub_activate_context(ctx->sub_ctx);


    RMC_LOG_COMMENT("sub[%d] pub[%d] node[%d] pub[%p] sub[%p]",
                    rmc_sub_get_socket_count(_dstc_default_context.sub_ctx),
                    rmc_pub_get_socket_count(_dstc_default_context.pub_ctx),
                    max_dstc_nodes,
                    _dstc_default_context.sub_ctx,
                    _dstc_default_context.pub_ctx);

    // Start ticking announcements as a client that the server will connect back to.
    // Only do announce if we have client services that requires servers to connect
    // back to us as a subsriber in order to make their remote functions available.
    if (_dstc_client_func_ind || _dstc_client_callback_count) {
        RMC_LOG_INFO("There are %d DSTC_CLIENT() and %d DSTC_CALLBACK() functions declared. Will send out announce.",
                     _dstc_client_func_ind, _dstc_client_callback_count);
        rmc_pub_set_announce_interval(ctx->pub_ctx, 200000); // Start ticking announces.
    }
    else
        RMC_LOG_INFO("No DSTC_CLIENT() or DSTC_CALLBACK() functions declared. Will not send out announce.");


    return 0;
}

rmc_node_id_t dstc_get_node_id(void)
{
    if (!_dstc_initialized)
        return 0;

    return rmc_pub_node_id(_dstc_default_context.pub_ctx);
}

int dstc_setup_epoll(int epoll_fd_arg)
{
    char* node_id = getenv(DSTC_ENV_NODE_ID);
    char* max_dstc_nodes = getenv(DSTC_ENV_MAX_NODES);
    char *multicast_group_addr = getenv(DSTC_ENV_MCAST_GROUP_ADDR);
    char *multicast_iface_addr = getenv(DSTC_ENV_MCAST_IFACE_ADDR);
    char *multicast_port = getenv(DSTC_ENV_MCAST_GROUP_PORT);
    char *control_listen_iface_addr = getenv(DSTC_ENV_CONTROL_LISTEN_IFACE);
    char *control_listen_port = getenv(DSTC_ENV_CONTROL_LISTEN_PORT);
    char *mcast_ttl = getenv(DSTC_ENV_MCAST_TTL);
    char *log_level = getenv(DSTC_ENV_LOG_LEVEL);

    if (_dstc_initialized)
        return EBUSY;

    _dstc_initialized = 1;

    rmc_set_log_level(log_level?atoi(log_level):RMC_LOG_LEVEL_ERROR);

    RMC_LOG_COMMENT("%s: %s", DSTC_ENV_NODE_ID, node_id?node_id:"[not set]");
    RMC_LOG_COMMENT("%s: %s", DSTC_ENV_MAX_NODES, max_dstc_nodes?max_dstc_nodes:"[not set]");
    RMC_LOG_COMMENT("%s: %s", DSTC_ENV_MCAST_GROUP_ADDR, multicast_group_addr?multicast_group_addr:"[not set]");
    RMC_LOG_COMMENT("%s: %s", DSTC_ENV_MCAST_IFACE_ADDR, multicast_iface_addr?multicast_iface_addr:"[not set]");
    RMC_LOG_COMMENT("%s: %s", DSTC_ENV_MCAST_GROUP_PORT, multicast_port?multicast_port:"[not set]");
    RMC_LOG_COMMENT("%s: %s", DSTC_ENV_MCAST_TTL, mcast_ttl?mcast_ttl:"[not set]");
    RMC_LOG_COMMENT("%s: %s", DSTC_ENV_CONTROL_LISTEN_IFACE, control_listen_iface_addr?control_listen_iface_addr:"[not set]");
    RMC_LOG_COMMENT("%s: %s", DSTC_ENV_CONTROL_LISTEN_PORT, control_listen_port?control_listen_port:"[not set]");

    return dstc_setup_internal(&_dstc_default_context,
                               (node_id?((rmc_node_id_t) strtoul(node_id, 0, 0)):0),
                               (max_dstc_nodes?atoi(max_dstc_nodes):DEFAULT_MAX_DSTC_NODES),
                               multicast_group_addr?multicast_group_addr:DEFAULT_MCAST_GROUP_ADDRESS,
                               (multicast_port?atoi(multicast_port):DEFAULT_MCAST_GROUP_PORT),
                               multicast_iface_addr,
                               (mcast_ttl?atoi(mcast_ttl):DEFAULT_MCAST_TTL),
                               control_listen_iface_addr,
                               (control_listen_port?atoi(control_listen_port):0),
                               epoll_fd_arg,
                               // user_data to be provided to poll_add, poll_modify, and poll_remove
                               user_data_nil());
}


int dstc_setup(void)
{
    if (_dstc_initialized)
        return EBUSY;

    return dstc_setup_epoll(epoll_create(1));
}

int dstc_setup2(int epoll_fd_arg,
                rmc_node_id_t node_id,
                int max_dstc_nodes,
                char* multicast_group_addr,
                int multicast_port,
                char* multicast_iface_addr,
                int mcast_ttl,
                char* control_listen_iface_addr,
                int control_listen_port,
                int log_level)
{
    if (_dstc_initialized)
        return EBUSY;

    _dstc_initialized = 1;
    rmc_set_log_level(log_level);

    return dstc_setup_internal(&_dstc_default_context,
                               node_id,
                               max_dstc_nodes,
                               multicast_group_addr,
                               multicast_port,
                               multicast_iface_addr,
                               mcast_ttl,
                               control_listen_iface_addr,
                               control_listen_port,
                               (epoll_fd_arg != -1)?epoll_fd_arg:epoll_create(1),
                               // user_data to be provided to poll_add, poll_modify, and poll_remove
                               user_data_nil());
}




static int dstc_queue(char* name, dstc_callback_t callback_ref, uint8_t* arg, uint32_t arg_sz)
{
    // Will be freed by RMC on confirmed delivery
    dstc_header_t *call = 0;
    uint16_t id_len = 0;
    size_t name_len = name?strlen(name):0;;

    if ((!name || name[0] == 0) && !callback_ref) {
        RMC_LOG_ERROR("dstc_queue() needs either name or callback_ref to be set.");
        return EINVAL;
    }

    // FIXME: Stuff multiple calls into a single packet.
    //        Queue packet either at timeout (1-2 msec) or when packet is full (RMC_MAX_PAYLOAD)
    if (!_dstc_initialized)
        dstc_setup();

    id_len = callback_ref?(sizeof(uint64_t) + 1):(name_len+1);
    call = (dstc_header_t*) dstc_payload_buffer_alloc(&_dstc_default_context,
                                                      sizeof(dstc_header_t) + id_len + arg_sz);

    // If alloca failed, then we do not have enough space in the payload buffer to store the new call.
    // Return EBUSY, telling the calling program to run dstc_process_events() or dstc_process_single_event()
    // for a bit and try again.
    if (!call)
        return EBUSY;


    call->node_id = dstc_get_node_id();

    // If this is a regular function call, then copy in the function
    // name, including terminating null character, followed by the
    // payload.
    //
    // If this is a invocation of a previously registered callback, then
    // install a null character as the first byte of payload, followed by
    // the eight bytes of the callback reference that we want invoked,
    // followed by the payload
    //
    if (name) {
        memcpy(call->payload, name, name_len + 1);
        call->payload_len = name_len + 1 + arg_sz;
        memcpy(call->payload + name_len + 1, arg, arg_sz);
    } else {
        call->payload[0] = 0;
        memcpy(call->payload + 1, (uint64_t*) &callback_ref, sizeof(uint64_t));
        call->payload_len = 1 + sizeof(uint64_t) + arg_sz;
        memcpy(call->payload + 1 + sizeof(uint64_t), arg, arg_sz);
    }

    RMC_LOG_DEBUG("DSTC Queue: node_id[%lu] name[%s]/callback_ref[%llu] payload_len[%d] in_use[%d]",
                  call->node_id,
                  name?name:"nil",
                  callback_ref,
                  call->payload_len,
                  dstc_payload_buffer_in_use(&_dstc_default_context));


    // If we have pending calls in the DSTC circular buffer, try to
    // queue them with RMC.  This may fail if we are currently
    // suspended from sending traffic over RMC due to congestion.
    //
    // The bottom line is that we will get low latency on single
    // calls, which will generate an RMC packet immediately, while
    // traffic suspension due to congestion leads to call bundling
    // into fewer but larger packets to increase efficiency.
    queue_pending_calls();

    return 0;
}


// Returns EBUSY if outbound queues are full
int dstc_queue_callback(dstc_callback_t addr, uint8_t* arg, uint32_t arg_sz)
{
    // Call with zero namelen to treat name as a 64bit integer.
    // This integer will be mapped by the received through the
    // _dstc_default_context.local_callback
    // table to a pending callback function.
    return dstc_queue(0, addr, arg, arg_sz);
}

// Returns EBUSY if outbound queues are full
int dstc_queue_func(char* name, uint8_t* arg, uint32_t arg_sz)
{
    return dstc_queue(name, 0, arg, arg_sz);
}
