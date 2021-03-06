/*
 * This file is distributed as part of the SkySQL Gateway.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright SkySQL Ab 2013
 */

/**
 * @file debugcmd.c  - The debug CLI command line interpreter
 *
 * The command interpreter for the dbug user interface. The command
 * structure is such that there are a numerb of commands, notably
 * show and a set of subcommands, the things to show in this case.
 *
 * Each subcommand has a handler function defined for it that is passeed
 * the DCB to use to print the output of the commands and up to 3 arguments
 * as numeric values.
 *
 * There are two "built in" commands, the help command and the quit
 * command.
 *
 * @verbatim
 * Revision History
 *
 * Date		Who			Description
 * 20/06/13	Mark Riddoch		Initial implementation
 * 17/07/13	Mark Riddoch		Additional commands
 * 09/08/2013	Massimiliano Pinto	Addes enable/disable commands (now only for log)
 *
 * @endverbatim
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <service.h>
#include <session.h>
#include <router.h>
#include <modules.h>
#include <atomic.h>
#include <server.h>
#include <spinlock.h>
#include <dcb.h>
#include <poll.h>
#include <users.h>
#include <dbusers.h>
#include <config.h>
#include <telnetd.h>
#include <adminusers.h>
#include <monitor.h>
#include <debugcli.h>

#include <skygw_utils.h>
#include <log_manager.h>

#define	MAXARGS	5

#define	ARG_TYPE_ADDRESS	1
#define	ARG_TYPE_STRING		2
/**
 * The subcommand structure
 *
 * These are the options that may be passed to a command
 */
struct subcommand {
	char	*arg1;
	int	n_args;
	void	(*fn)();
	char	*help;
	int	arg_types[3];
};

static	void	telnetdShowUsers(DCB *);
/**
 * The subcommands of the show command
 */
struct subcommand showoptions[] = {
        { "dcbs",	0, dprintAllDCBs,	"Show all descriptor control blocks (network connections)",
				{0, 0, 0} },
	{ "dcb",	1, dprintDCB,		"Show a single descriptor control block e.g. show dcb 0x493340",
				{ARG_TYPE_ADDRESS, 0, 0} },
	{ "dbusers",	1, dcb_usersPrint,	"Show statistics and user names for a service's user table.\n\t\tExample : show dbusers <ptr of 'User's data' from services list>",
				{ARG_TYPE_ADDRESS, 0, 0} },
	{ "epoll",	0, dprintPollStats,	"Show the poll statistics",
				{0, 0, 0} },
	{ "modules",	0, dprintAllModules,	"Show all currently loaded modules",
				{0, 0, 0} },
	{ "monitors",	0, monitorShowAll,	"Show the monitors that are configured",
				{0, 0, 0} },
	{ "server",	1, dprintServer,	"Show details for a server, e.g. show server 0x485390",
				{ARG_TYPE_ADDRESS, 0, 0} },
	{ "servers",	0, dprintAllServers,	"Show all configured servers",
				{0, 0, 0} },
	{ "services",	0, dprintAllServices,	"Show all configured services in MaxScale",
				{0, 0, 0} },
	{ "session",	1, dprintSession, 	"Show a single session in MaxScale, e.g. show session 0x284830",
				{ARG_TYPE_ADDRESS, 0, 0} },
	{ "sessions",	0, dprintAllSessions, 	"Show all active sessions in MaxScale",
				{0, 0, 0} },
	{ "users",	0, telnetdShowUsers,	"Show statistics and user names for the debug interface",
				{ARG_TYPE_ADDRESS, 0, 0} },
	{ NULL,		0, NULL,		NULL,
				{0, 0, 0} }
};

extern void shutdown_server();
static void shutdown_service(DCB *dcb, SERVICE *service);
static void shutdown_monitor(DCB *dcb, MONITOR *monitor);

/**
 * The subcommands of the shutdown command
 */
struct subcommand shutdownoptions[] = {
	{ "maxscale",
          0,
          shutdown_server,
          "Shutdown MaxScale",

          {0, 0, 0}
        },
	{
            "monitor",
            1,
            shutdown_monitor,
            "Shutdown a monitor, e.g. shutdown monitor 0x48381e0",
            {ARG_TYPE_ADDRESS, 0, 0}
        },
	{
            "service",
            1,
            shutdown_service,
            "Shutdown a service, e.g. shutdown service 0x4838320",
            {ARG_TYPE_ADDRESS, 0, 0}
        },
	{
            NULL,
            0,
            NULL,
            NULL,
            {0, 0, 0}
        }
};


static void restart_service(DCB *dcb, SERVICE *service);
static void restart_monitor(DCB *dcb, MONITOR *monitor);
/**
 * The subcommands of the restart command
 */
struct subcommand restartoptions[] = {
	{ "monitor",	1, restart_monitor,	"Restart a monitor, e.g. restart monitor 0x48181e0",
				{ARG_TYPE_ADDRESS, 0, 0} },
	{ "service",	1, restart_service,	"Restart a service, e.g. restart service 0x4838320",
				{ARG_TYPE_ADDRESS, 0, 0} },
	{ NULL,		0, NULL,		NULL,
				{0, 0, 0} }
};

static void set_server(DCB *dcb, SERVER *server, char *bit);
/**
 * The subcommands of the set command
 */
struct subcommand setoptions[] = {
	{ "server",	2, set_server,	"Set the status of a server. E.g. set server 0x4838320 master",
				{ARG_TYPE_ADDRESS, ARG_TYPE_STRING, 0} },
	{ NULL,		0, NULL,		NULL,
				{0, 0, 0} }
};

static void clear_server(DCB *dcb, SERVER *server, char *bit);
/**
 * The subcommands of the clear command
 */
struct subcommand clearoptions[] = {
	{ "server",	2, clear_server,	"Clear the status of a server. E.g. clear server 0x4838320 master",
				{ARG_TYPE_ADDRESS, ARG_TYPE_STRING, 0} },
	{ NULL,		0, NULL,		NULL,
				{0, 0, 0} }
};

static void reload_dbusers(DCB *dcb, SERVICE *service);
static void reload_config(DCB *dcb);

/**
 * The subcommands of the reload command
 */
struct subcommand reloadoptions[] = {
	{ "config",	0, reload_config,	"Reload the configuration data for MaxScale.",
				{ARG_TYPE_ADDRESS, 0, 0} },
	{ "dbusers",	1, reload_dbusers,	"Reload the dbuser data for a service. E.g. reload dbusers 0x849420",
				{ARG_TYPE_ADDRESS, 0, 0} },
	{ NULL,		0, NULL,		NULL,
				{0, 0, 0} }
};

static void enable_log_action(DCB *, char *);
static void disable_log_action(DCB *, char *);

/**
 *  * The subcommands of the enable command
 *   */
struct subcommand enableoptions[] = {
        {
                "log",
                1,
                enable_log_action,
                "Enable Log options for MaxScale, options trace | error | "
                "message E.g. enable log message.",
                {ARG_TYPE_STRING, 0, 0}
        },
        {
                NULL,
                0,
                NULL,
                NULL,
                {0, 0, 0}
        }
};


/**
 *  * The subcommands of the disable command
 *   */
struct subcommand disableoptions[] = {
    {
            "log",
            1,
            disable_log_action,
            "Disable Log for MaxScale, Options: debug | trace | error | message "
            "E.g. disable log debug",
            {ARG_TYPE_STRING, 0, 0}
    },
    {
            NULL,
            0,
            NULL,
            NULL,
            {0, 0, 0}
    }
};

#if defined(SS_DEBUG)

static void fail_backendfd(void);
static void fail_clientfd(void);
static void fail_accept(DCB* dcb, char* arg1, char* arg2);
/**
 *  * The subcommands of the fail command
 *   */
struct subcommand failoptions[] = {
    {
        "backendfd",
        0,
        fail_backendfd,
        "Fail backend socket for next operation.",
        {ARG_TYPE_STRING, 0, 0}
    },
    {
        "clientfd",
        0,
        fail_clientfd,
        "Fail client socket for next operation.",
        {ARG_TYPE_STRING, 0, 0}
    },
    {
        "accept",
        2,
        fail_accept,
        "Fail to accept next client connection.",
        {ARG_TYPE_STRING, ARG_TYPE_STRING, 0}
    },
    {
        NULL,
        0,
        NULL,
        NULL,
        {0, 0, 0}
    }
};
#endif /* SS_DEBUG */

static void telnetdAddUser(DCB *, char *, char *);
/**
 * The subcommands of the add command
 */
struct subcommand addoptions[] = {
	{ "user",	2, telnetdAddUser,	"Add a new user for the debug interface. E.g. add user john today",
				{ARG_TYPE_STRING, ARG_TYPE_STRING, 0} },
	{ NULL,		0, NULL,		NULL,
				{0, 0, 0} }
};


static void telnetdRemoveUser(DCB *, char *, char *);
/**
 * The subcommands of the remove command
 */
struct subcommand removeoptions[] = {
	{
            "user",
            2,
            telnetdRemoveUser,
            "Remove existing maxscale user. Example : remove user john johnpwd",
            {ARG_TYPE_STRING, ARG_TYPE_STRING, 0}
        },
	{
            NULL, 0, NULL, NULL, {0, 0, 0}
        }
};


/**
 * The debug command table
 */
static struct {
	char			*cmd;
	struct	subcommand	*options;
} cmds[] = {
	{ "add",	addoptions },
	{ "clear",	clearoptions },
        { "remove",     removeoptions },
	{ "restart",	restartoptions },
	{ "set",	setoptions },
	{ "show",	showoptions },
	{ "shutdown",	shutdownoptions },
	{ "reload",	reloadoptions },
	{ "enable",     enableoptions },
	{ "disable",    disableoptions },
#if defined(SS_DEBUG)
        { "fail",       failoptions },
#endif
	{ NULL,		NULL	}
};


/**
 * Convert a string argument to a numeric, observing prefixes
 * for number bases, e.g. 0x for hex, 0 for octal
 *
 * @param arg		The string representation of the argument
 * @param arg_type	The target type for the argument
 * @return The argument as a long integer
 */
static unsigned long
convert_arg(char *arg, int arg_type)
{
	switch (arg_type)
	{
	case ARG_TYPE_ADDRESS:
		return (unsigned long)strtol(arg, NULL, 0);
	case ARG_TYPE_STRING:
		return (unsigned long)arg;
	}
	return 0;
}

/**
 * We have a complete line from the user, lookup the commands and execute them
 *
 * Commands are tokenised based on white space and then the first
 * word is checked againts the cmds table. If a match is found the
 * second word is compared to the different options for that command.
 *
 * Commands may also take up to 3 additional arguments, these are all
 * assumed to the numeric values and will be converted before being passed
 * to the handler function for the command.
 *
 * @param cli		The CLI_SESSION
 * @return	Returns 0 if the interpreter should exit
 */
int
execute_cmd(CLI_SESSION *cli)
{
DCB		*dcb = cli->session->client;
int		argc, i, j, found = 0;
char		*args[MAXARGS];
char		*saveptr, *delim = " \t\r\n";
unsigned long	arg1, arg2, arg3;

	/* Tokenize the input string */
	args[0] = strtok_r(cli->cmdbuf, delim, &saveptr);
	i = 0;
	do {
		i++;
		args[i] = strtok_r(NULL, delim, &saveptr);
	} while (args[i] != NULL && i < MAXARGS);

	if (args[0] == NULL)
		return 1;
	argc = i - 2;	/* The number of extra arguments to commands */
	

	if (!strcasecmp(args[0], "help"))
	{
		if (args[1] == NULL)
		{
			found = 1;
			dcb_printf(dcb, "Available commands:\n");
			for (i = 0; cmds[i].cmd; i++)
			{
				for (j = 0; cmds[i].options[j].arg1; j++)
				{
					dcb_printf(dcb, "    %s %s\n", cmds[i].cmd, cmds[i].options[j].arg1);
				}
			}
		}
		else
		{
			for (i = 0; cmds[i].cmd; i++)
			{
				if (!strcasecmp(args[1], cmds[i].cmd))
				{
					found = 1;
					dcb_printf(dcb, "Available options to the %s command:\n", args[1]);
					for (j = 0; cmds[i].options[j].arg1; j++)
					{
						dcb_printf(dcb, "    %-10s %s\n", cmds[i].options[j].arg1,
										cmds[i].options[j].help);
					}
				}
			}
			if (found == 0)
			{
				dcb_printf(dcb, "No command %s to offer help with\n", args[1]);
			}
		}
		found = 1;
	}
	else if (!strcasecmp(args[0], "quit"))
	{
		return 0;
	}
	else if (argc >= 0)
	{
		for (i = 0; cmds[i].cmd; i++)
		{
			if (strcasecmp(args[0], cmds[i].cmd) == 0)
			{
				for (j = 0; cmds[i].options[j].arg1; j++)
				{
                                        found = 1; /**< command and sub-command match */
					if (strcasecmp(args[1], cmds[i].options[j].arg1) == 0)
					{
						if (argc != cmds[i].options[j].n_args)
						{
							dcb_printf(dcb, "Incorrect number of arguments: %s %s expects %d arguments\n",
								cmds[i].cmd, cmds[i].options[j].arg1,
								cmds[i].options[j].n_args);
							
						}
						else
						{
							switch (cmds[i].options[j].n_args)
							{
							case 0:
								cmds[i].options[j].fn(dcb);
								break;
							case 1:
								arg1 = convert_arg(args[2],cmds[i].options[j].arg_types[0]);
								if (arg1)
									cmds[i].options[j].fn(dcb, arg1);
								else
									dcb_printf(dcb, "Invalid argument: %s\n",
										args[2]);
								break;
							case 2:
								arg1 = convert_arg(args[2],cmds[i].options[j].arg_types[0]);
								arg2 = convert_arg(args[3],cmds[i].options[j].arg_types[1]);
								if (arg1 && arg2)
									cmds[i].options[j].fn(dcb, arg1, arg2);
								else if (arg1 == 0)
									dcb_printf(dcb, "Invalid argument: %s\n",
										args[2]);
								else
									dcb_printf(dcb, "Invalid argument: %s\n",
										args[3]);
								break;
							case 3:
								arg1 = convert_arg(args[2],cmds[i].options[j].arg_types[0]);
								arg2 = convert_arg(args[3],cmds[i].options[j].arg_types[1]);
								arg3 = convert_arg(args[4],cmds[i].options[j].arg_types[2]);
								if (arg1 && arg2 && arg3)
									cmds[i].options[j].fn(dcb, arg1, arg2, arg3);
								else if (arg1 == 0)
									dcb_printf(dcb, "Invalid argument: %s\n",
										args[2]);
								else if (arg2 == 0)
									dcb_printf(dcb, "Invalid argument: %s\n",
										args[3]);
								else if (arg3 == 0)
									dcb_printf(dcb, "Invalid argument: %s\n",
										args[4]);
							}
						}
					}
				}
				if (!found)
				{
					dcb_printf(dcb,
						"Unknown or missing option for the %s command. Valid sub-commands are:\n",
							cmds[i].cmd);
					for (j = 0; cmds[i].options[j].arg1; j++)
					{
						dcb_printf(dcb, "    %-10s %s\n", cmds[i].options[j].arg1,
										cmds[i].options[j].help);
					}
					found = 1;
				}
			}
		}
	}
	else if (argc == -1)
	{
		dcb_printf(dcb,
			"Commands must consist of at least two words. Type help for a list of commands\n");
		found = 1;
	}
	if (!found)
		dcb_printf(dcb,
			"Command '%s' not known, type help for a list of available commands\n", args[0]);
	memset(cli->cmdbuf, 0, 80);

	return 1;
}

/**
 * Debug command to stop a service
 *
 * @param dcb		The DCB to print any output to
 * @param service	The service to shutdown
 */
static void
shutdown_service(DCB *dcb, SERVICE *service)
{
	serviceStop(service);
}

/**
 * Debug command to restart a stopped service
 *
 * @param dcb		The DCB to print any output to
 * @param service	The service to restart
 */
static void
restart_service(DCB *dcb, SERVICE *service)
{
	serviceRestart(service);
}

static struct {
	char		*str;
	unsigned int	bit;
} ServerBits[] = {
	{ "running", 	SERVER_RUNNING },
	{ "master",	SERVER_MASTER },
	{ "slave",	SERVER_SLAVE },
	{ "synced",	SERVER_JOINED },
	{ NULL,		0 }
};
/**
 * Map the server status bit
 *
 * @param str	String representation
 * @return bit value or 0 on error
 */
static unsigned int
server_map_status(char *str)
{
int i;

	for (i = 0; ServerBits[i].str; i++)
		if (!strcasecmp(str, ServerBits[i].str))
			return ServerBits[i].bit;
	return 0;
}

/**
 * Set the status bit of a server
 *
 * @param dcb		DCB to send output to
 * @param server	The server to set the status of
 * @param bit		String representation of the status bit
 */
static void
set_server(DCB *dcb, SERVER *server, char *bit)
{
unsigned int bitvalue;

	if ((bitvalue = server_map_status(bit)) != 0)
		server_set_status(server, bitvalue);
	else
		dcb_printf(dcb, "Unknown status bit %s\n", bit);
}


/**
 * Clear the status bit of a server
 *
 * @param dcb		DCB to send output to
 * @param server	The server to set the status of
 * @param bit		String representation of the status bit
 */
static void
clear_server(DCB *dcb, SERVER *server, char *bit)
{
unsigned int bitvalue;

	if ((bitvalue = server_map_status(bit)) != 0)
		server_clear_status(server, bitvalue);
	else
		dcb_printf(dcb, "Unknown status bit %s\n", bit);
}

/**
 * Reload the authenticaton data from the backend database of a service.
 *
 * @param dcb		DCB to send output
 * @param service	The service to update
 */
static void
reload_dbusers(DCB *dcb, SERVICE *service)
{
	dcb_printf(dcb, "Loaded %d database users for server %s.\n",
			reload_mysql_users(service), service->name);
}

/**
 * Relaod the configuration data from the config file
 *
 * @param dcb		DCB to use to send output
 */
static void
reload_config(DCB *dcb)
{
	dcb_printf(dcb, "Reloading configuration from file.\n");
	config_reload();
}

/**
 * Add a new maxscale admin user
 *
 * @param dcb		The DCB for messages
 * @param user		The user name
 * @param passwd	The Password of the user
 */
static void
telnetdAddUser(DCB *dcb, char *user, char *passwd)
{
char	*err;

	if (admin_search_user(user))
	{
		dcb_printf(dcb, "User %s already exists.\n", user);
		return;
	}
	if ((err = admin_add_user(user, passwd)) == NULL)
		dcb_printf(dcb, "User %s has been successfully added.\n", user);
	else
		dcb_printf(dcb, "Failed to add new user. %s\n", err);
}


/**
 * Remove a maxscale admin user
 *
 * @param dcb		The DCB for messages
 * @param user		The user name
 * @param passwd	The Password of the user
 */
static void telnetdRemoveUser(
        DCB*  dcb,
        char* user,
        char* passwd)
{
        char* err;

	if (!admin_search_user(user))
        {
            dcb_printf(dcb, "User %s doesn't exist.\n", user);
            return;
        }
        
	if ((err = admin_remove_user(user, passwd)) == NULL)
        {
            dcb_printf(dcb, "User %s has been successfully removed.\n", user);
        }
        else
        {
            dcb_printf(dcb, "Failed to remove user %s. %s\n", user, err);
        }
}



/**
 * Print the adminsitration users
 *
 * @param dcb	The DCB to print the user data to
 */
static void
telnetdShowUsers(DCB *dcb)
{
	dcb_printf(dcb, "Administration interface users:\n");
	dcb_PrintAdminUsers(dcb);
}

/**
 * Command to shutdown a running monitor
 *
 * @param dcb	The DCB to use to print messages
 * @param monitor	The monitor to shutdown
 */
static void
shutdown_monitor(DCB *dcb, MONITOR *monitor)
{
	monitorStop(monitor);
}

/**
 * Command to restart a stopped monitor
 *
 * @param dcb	The DCB to use to print messages
 * @param monitor	The monitor to restart
 */
static void
restart_monitor(DCB *dcb, MONITOR *monitor)
{
	monitorStart(monitor);
}

/**
 * The log enable action
 */

static void enable_log_action(DCB *dcb, char *arg1) {
        logfile_id_t type;
        int max_len = strlen("message");

        if (strncmp(arg1, "debug", max_len) == 0) {
                type = LOGFILE_DEBUG;
        } else if (strncmp(arg1, "trace", max_len) == 0) {
                type = LOGFILE_TRACE;
        } else if (strncmp(arg1, "error", max_len) == 0) {
                type = LOGFILE_ERROR;
        } else if (strncmp(arg1, "message", max_len) == 0) {
                type = LOGFILE_MESSAGE;
        } else {
                dcb_printf(dcb, "%s is not supported for enable log\n", arg1);
                return ;
        }

        skygw_log_enable(type);
}

/**
 * The log disable action
 */

static void disable_log_action(DCB *dcb, char *arg1) {
        logfile_id_t type;
        int max_len = strlen("message");

        if (strncmp(arg1, "debug", max_len) == 0) {
                type = LOGFILE_DEBUG;
        } else if (strncmp(arg1, "trace", max_len) == 0) {
                type = LOGFILE_TRACE;
        } else if (strncmp(arg1, "error", max_len) == 0) {
                type = LOGFILE_ERROR;
        } else if (strncmp(arg1, "message", max_len) == 0) {
                type = LOGFILE_MESSAGE;
        } else {
                dcb_printf(dcb, "%s is not supported for disable log\n", arg1);
                return ;
        }

        skygw_log_disable(type);
}

#if defined(SS_DEBUG)
static void fail_backendfd(void)
{ 
        fail_next_backend_fd = true;
}

static void fail_clientfd(void)
{ 
        fail_next_client_fd = true;
}

static void fail_accept(
        DCB*  dcb,
        char* arg1,
        char* arg2)
{
        int failcount = MIN(atoi(arg2), 100);
        fail_accept_errno = atoi(arg1);


        switch(fail_accept_errno) {
                case EAGAIN:
//                case EWOULDBLOCK:
                case EBADF:
                case EINTR:
                case EINVAL:
                case EMFILE:
                case ENFILE:
                case ENOTSOCK:
                case EOPNOTSUPP:
                case ENOBUFS:
                case ENOMEM:
                case EPROTO:
                        fail_next_accept = failcount;
        break;

                default:
                        dcb_printf(dcb,
                                   "[%d, %s] is not valid errno for accept.\n",
                                   fail_accept_errno,
                                   strerror(fail_accept_errno));
                return ;
        }
}
#endif
