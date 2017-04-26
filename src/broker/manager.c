/*
 * Bus Manager
 */

#include <c-list.h>
#include <c-macro.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "connection.h"
#include "main.h"
#include "manager.h"
#include "user.h"
#include "util/dispatch.h"
#include "util/error.h"

struct Manager {
        UserRegistry users;
        DispatchContext dispatcher;
        CList dispatcher_list;
        CList dispatcher_hup;

        int signals_fd;
        DispatchFile signals_file;

        Connection controller;
};

static int manager_dispatch_signals(DispatchFile *file, uint32_t events) {
        Manager *manager = c_container_of(file, Manager, signals_file);
        struct signalfd_siginfo si;
        ssize_t l;

        assert(events == EPOLLIN);

        l = read(manager->signals_fd, &si, sizeof(si));
        if (l < 0)
                return error_origin(-errno);

        assert(l == sizeof(si));

        if (main_arg_verbose)
                fprintf(stderr,
                        "Caught %s, exiting\n",
                        (si.ssi_signo == SIGTERM ? "SIGTERM" :
                         si.ssi_signo == SIGINT ? "SIGINT" :
                         "SIG?"));

        return DISPATCH_E_EXIT;
}

static int manager_dispatch_controller(DispatchFile *file, uint32_t events) {
        return 0;
}

int manager_new(Manager **managerp, int controller_fd) {
        _c_cleanup_(manager_freep) Manager *manager = NULL;
        _c_cleanup_(user_entry_unrefp) UserEntry *user = NULL;
        struct ucred ucred;
        socklen_t z_ucred;
        sigset_t sigmask;
        int r;

        r = getsockopt(controller_fd, SOL_SOCKET, SO_PEERCRED, &ucred, &z_ucred);
        if (r < 0)
                return error_origin(-errno);

        manager = calloc(1, sizeof(*manager));
        if (!manager)
                return error_origin(-ENOMEM);

        user_registry_init(&manager->users, 16 * 1024 * 1024, 128, 128, 128, 128);
        manager->dispatcher = (DispatchContext)DISPATCH_CONTEXT_NULL;
        manager->dispatcher_list = (CList)C_LIST_INIT(manager->dispatcher_list);
        manager->dispatcher_hup = (CList)C_LIST_INIT(manager->dispatcher_hup);
        manager->signals_fd = -1;
        manager->signals_file = (DispatchFile)DISPATCH_FILE_NULL(manager->signals_file);
        manager->controller = (Connection)CONNECTION_NULL(manager->controller);

        r = dispatch_context_init(&manager->dispatcher);
        if (r)
                return error_fold(r);

        sigemptyset(&sigmask);
        sigaddset(&sigmask, SIGTERM);
        sigaddset(&sigmask, SIGINT);

        manager->signals_fd = signalfd(-1, &sigmask, SFD_CLOEXEC | SFD_NONBLOCK);
        if (manager->signals_fd < 0)
                return error_origin(-errno);

        r = dispatch_file_init(&manager->signals_file,
                               &manager->dispatcher,
                               &manager->dispatcher_list,
                               manager_dispatch_signals,
                               manager->signals_fd,
                               EPOLLIN);
        if (r)
                return error_fold(r);

        r = user_registry_ref_entry(&manager->users, &user, ucred.uid);
        if (r)
                return error_fold(r);

        r = connection_init_server(&manager->controller,
                                   &manager->dispatcher,
                                   &manager->dispatcher_list,
                                   &manager->dispatcher_hup,
                                   manager_dispatch_controller,
                                   user,
                                   "0123456789abcdef",
                                   controller_fd);
        if (r)
                return error_fold(r);

        dispatch_file_select(&manager->signals_file, EPOLLIN);

        *managerp = manager;
        manager = NULL;
        return 0;
}

Manager *manager_free(Manager *manager) {
        if (!manager)
                return NULL;

        connection_deinit(&manager->controller);
        dispatch_file_deinit(&manager->signals_file);
        c_close(manager->signals_fd);
        assert(c_list_is_empty(&manager->dispatcher_hup));
        assert(c_list_is_empty(&manager->dispatcher_list));
        dispatch_context_deinit(&manager->dispatcher);
        user_registry_deinit(&manager->users);
        free(manager);

        return NULL;
}

static int manager_hangup(Manager *manager, Connection *connection) {
        /*
         * A hangup on the controller causes a shutdown of the broker. However,
         * we always flush out all pending output buffers, before we exit.
         * Hence, we wait until the controller-connection is fully done.
         */
        if (connection == &manager->controller)
                return connection_is_running(connection) ? 0 : MAIN_EXIT;

        return 0;
}

static int manager_dispatch(Manager *manager) {
        CList processed = (CList)C_LIST_INIT(processed);
        Connection *connection;
        DispatchFile *file;
        int r;

        r = dispatch_context_poll(&manager->dispatcher, c_list_is_empty(&manager->dispatcher_list) ? -1 : 0);
        if (r)
                return error_fold(r);

        do {
                while (!r && (connection = c_list_first_entry(&manager->dispatcher_hup, Connection, hup_link))) {
                        c_list_unlink_init(&connection->hup_link);
                        r = error_trace(manager_hangup(manager, connection));
                }

                while (!r &&
                       c_list_is_empty(&manager->dispatcher_hup) &&
                       (file = c_list_first_entry(&manager->dispatcher_list, DispatchFile, ready_link))) {

                        /*
                         * Whenever we dispatch an entry, we first move it into
                         * a separate list, so if it modifies itself or others,
                         * it will not corrupt our list iterator.
                         *
                         * Then we call into is dispatcher, so it can handle
                         * the I/O events. The dispatchers can use MAIN_EXIT or
                         * MAIN_FAILURE to exit the main-loop. Everything else
                         * is treated as fatal.
                         *
                         * Additionally to this ready-list, we have a
                         * hangup-list, which is a high-priority list. Whenever
                         * a dispatcher needs to disconnect its current
                         * connection, or any remote connection, it can put
                         * those on the hangup-list, and they are guaranteed to
                         * be handled next, before we continue with the normal
                         * ready-list.
                         * This is needed to avoid generating
                         * disconnect-signals from deep code-paths all over the
                         * place. We instead always defer the disconnect
                         * handling to the hangup-list.
                         */

                        c_list_unlink(&file->ready_link);
                        c_list_link_tail(&processed, &file->ready_link);

                        r = dispatch_file_call(file);
                        if (r == DISPATCH_E_EXIT)
                                r = MAIN_EXIT;
                        else if (r == DISPATCH_E_FAILURE)
                                r = MAIN_FAILED;
                        else
                                r = error_fold(r);
                }
        } while (!r);

        c_list_splice(&manager->dispatcher_list, &processed);
        return r;
}

int manager_run(Manager *manager) {
        sigset_t signew, sigold;
        int r;

        sigemptyset(&signew);
        sigaddset(&signew, SIGTERM);
        sigaddset(&signew, SIGINT);

        sigprocmask(SIG_BLOCK, &signew, &sigold);

        do {
                r = manager_dispatch(manager);
        } while (!r);

        sigprocmask(SIG_SETMASK, &sigold, NULL);

        return error_trace(r);
}
