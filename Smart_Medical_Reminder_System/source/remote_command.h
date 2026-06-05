#ifndef REMOTE_COMMAND_H_
#define REMOTE_COMMAND_H_

#include <stdbool.h>

/*
 * Handle one remote command received from the server.
 *
 * For now:
 *   ACK      -> acknowledge active pre-alert/reminder
 *
 * Returns true if the command was recognized and handled.
 * Returns false if the command was invalid, unsupported,
 * or could not be applied in the current state.
 */
bool RemoteCommand_Handle(const char *cmd);

#endif
