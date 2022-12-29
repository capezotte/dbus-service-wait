#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h> // alarm function
#include <sys/wait.h> // waitpid function
#include <dbus/dbus.h>

#include "arg.h"


typedef struct {
	/** Holds name of service to wait for */
	char *expected_name;
	/** Holds notification-fd */
	FILE *notify;
	/** Holds pending ListNames method call */
	DBusPendingCall *pending;
	/** Holds executable name (argv[0]) */
	char *arg0;
} ProgramData;

#define DIE_HELP exit(help(stderr, &data))

/**
 * Filters recieved messages and calls request_service_list to (re)check list
 * of available services every time when NameOwnerChanged event is detected.
 */
static DBusHandlerResult check_nameowner_changed (DBusConnection *bus, DBusMessage *message, void *user_data);

/**
 * Sends ListNames method call to request list of all available services.
 */
static void request_service_list(DBusConnection *bus, ProgramData *data);

/**
 * Parses list of services recieved as response to method call
 * sent by request_service_list.
 */
static void parse_service_list(ProgramData *data);

/** Called by SIGALRM if service is not found in given time */
static void timeout_reached(int sig);

/** Prints error message. Returns 1 */
static int help(FILE *fd, ProgramData *data);

char *argv0;

int main (int argc, char *argv[]) {
	ProgramData data = { NULL, NULL, NULL, argv[0] };
	unsigned int timeout = 5;
	int write_to = -1;

	// Parse arguments
	ARGBEGIN {
	case 't':
		timeout = atoi(EARGF(DIE_HELP));
		if (timeout <= 0) DIE_HELP;
		break;
	case 'h':
		help(stdout, &data);
		return 0;
	case 'd':
		write_to = atoi(EARGF(DIE_HELP));
		if (write_to < 3) DIE_HELP;

		if ((data.notify = fdopen(write_to, "w")) == NULL) {
			perror("fdopen");
			return 1;
		}
	} ARGEND;

	if (argc > 0) {
		data.expected_name = *argv;
		argv++; argc--;
	} else {
		DIE_HELP;
	}

	if (argc > 0) {
		pid_t fork1 = fork ();
		switch (fork1) {
			case -1:
				perror ("fork (step 1)");
				return 1;
			case 0:
				switch (fork ()) {
					case -1:
						perror ("fork (step 2)");
						return 1;
					case 0:
						break;
					default:
						return 0;
				}
				break;
			default:
				waitpid(fork1, NULL, 0);
				if (data.notify) fclose(data.notify);
				execvp(*argv, argv);
				return 1;
		}
	}

	DBusError error = DBUS_ERROR_INIT;
	DBusConnection *bus;

	// Setup timeout
	signal (SIGALRM, timeout_reached);
	alarm (timeout);

	// Connect to DBus
	char *busname;
	if (getenv ("DBUS_SESSION_BUS_ADDRESS")) {
		bus = dbus_bus_get (DBUS_BUS_SESSION, &error);
		busname = "session";
	} else {
		bus = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
		busname = "system";
	}
	if (!bus) {
		fprintf (stderr, "%s: Failed to acquire %s bus: %s\n", argv[0], busname, error.message);
		dbus_error_free (&error);
		return 1;
	}

	// Setup filtering
	char* filter = "interface='org.freedesktop.DBus',member='NameOwnerChanged'";
	dbus_bus_add_match (bus, filter, &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s: Failed to setup filter: %s\n", argv[0], error.message);
		dbus_error_free (&error);
		return 1;
	}

	// Enable calling to check_nameowner_changed
	dbus_connection_add_filter (bus, check_nameowner_changed, &data, NULL);
	request_service_list(bus, &data);

	// Loop until finished
	while (dbus_connection_read_write_dispatch (bus, -1)) {
		if ((data.pending != NULL) && dbus_pending_call_get_completed(data.pending)) {
			parse_service_list(&data);
			dbus_pending_call_unref(data.pending);
			data.pending = NULL;
		}
	}

	dbus_connection_unref (bus);
	return 0;
}


static void timeout_reached(int s) {
	fprintf (stderr, "Timeout reached\n");
	exit(1);
}


static void request_service_list(DBusConnection *bus, ProgramData* data) {
	DBusMessage* message;

	message = dbus_message_new_method_call("org.freedesktop.DBus",
		"/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames");
	if (message == NULL) {
		fprintf (stderr, "%s: Failed to allocate dbus message\n", data->arg0);
		exit(1);
	}

	if (!dbus_connection_send_with_reply (bus, message, &(data->pending), -1)) {
		fprintf (stderr, "%s: Failed to send dbus message\n", data->arg0);
		exit(1);
	}
	dbus_connection_flush(bus);

	if (NULL == data->pending) {
		fprintf (stderr, "%s: Failed to allocate response for ListNames call\n", data->arg0);
		exit(1);
	}

	dbus_message_unref(message);
}


static void parse_service_list(ProgramData *data) {
	DBusMessageIter iter;
	DBusMessageIter value;
	DBusMessage *message = dbus_pending_call_steal_reply(data->pending);

	dbus_message_iter_init(message, &iter);

	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_recurse(&iter, &value);
		while (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
			const char *name;
			dbus_message_iter_get_basic(&value, &name);
			if (strcmp(data->expected_name, name) == 0) {
				if (data->notify)
					fputs("\n", data->notify);
					fclose(data->notify);
				exit(0);
			}
			dbus_message_iter_next(&value);
		}
	}

	dbus_message_unref(message);

}


static DBusHandlerResult check_nameowner_changed (DBusConnection *bus, DBusMessage *message, void* user_data) {
	ProgramData *data = (ProgramData*)user_data;
	DBusMessageIter iter;
	int i = 0;
	char* str = NULL;

	// Watch only for NameOwnerChanged messages
	if (dbus_message_has_member(message, "NameOwnerChanged")) {
		if (data->pending == NULL) {
			request_service_list(bus, data);
		}
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}


static int help(FILE *fd, ProgramData *data) {
	fprintf (fd, "Usage: %s [-t timeout] [-d fd] BusName [prog...]\n", data->arg0);
	fprintf (fd, "   or: %s [-h]\n", data->arg0);
	fprintf (fd, "\n");
	fprintf (fd, "Waits until specified the D-Bus bus name is acquired. If prog is provided, the waiting will be done in a grandchild of prog.\n");
	fprintf (fd, "Options:\n");
	fprintf (fd, "  -t            Specifies timeout in seconds. Default 5\n");
	fprintf (fd, "  -h            Display this help output\n");
	fprintf (fd, "  -d            Write newline to this fd upon service being found. Must be greater than 2\n");
	fprintf (fd, "\n");
	fprintf (fd, "Exits codes:\n");
	fprintf (fd, "  0 - name is acquired\n");
	fprintf (fd, "  1 - timeout was reached or other failure.\n");

	return 1;
}
