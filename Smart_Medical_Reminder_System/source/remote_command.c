#include "remote_command.h"

#include <string.h>
#include <stdlib.h>
#include "fsl_debug_console.h"
#include "reminder.h"
#include "dispense_verify.h"
#include "server_logger.h"
#include "weight.h"


//-----------------------------------




bool RemoteCommand_Handle(const char *cmd)
{
    if (cmd == NULL)
    {
        PRINTF("SYS,REMOTE_CMD,NULL\r\n");
        return false;
    }

    if (strcmp(cmd, "ACK") == 0)
    {
        PRINTF("SYS,REMOTE_CMD_RX,ACK\r\n");

        if (Reminder_RemoteAck())
        {
            PRINTF("SYS,REMOTE_CMD_DONE,ACK,APPLIED\r\n");
            return true;
        }
        else
        {
            PRINTF("SYS,REMOTE_CMD_IGNORED,ACK\r\n");
            return false;
        }
    }

    if (strcmp(cmd, "SNOOZE") == 0)
    {
        PRINTF("SYS,REMOTE_CMD_RX,SNOOZE\r\n");

        if (Reminder_RemoteSnooze())
        {
            DoseEvent evt;

            if (Reminder_GetActiveDoseEvent(&evt))
            {
                (void)ServerLogger_SendDoseEvent(
                    evt.med_id,
                    "SNOOZED",
                    evt.reminder_count,
                    evt.delay_minutes
                );
            }

            PRINTF("SYS,REMOTE_CMD_DONE,SNOOZE,APPLIED\r\n");
            return true;
        }
        else
        {
            PRINTF("SYS,REMOTE_CMD_IGNORED,SNOOZE\r\n");
            return false;
        }
    }

    /*
     * Digital twin TAKEN simulation:
     * instead of forcing TAKEN directly, inject a virtual tray weight.
     * Example command:
     *   TWIN_WEIGHT:0.2
     *
     * Reminder logic will then decide TAKEN/MISSED naturally
     * using the same weight-based path as real hardware.
     */
    if (strncmp(cmd, "TWIN_WEIGHT:", 12) == 0)
    {
        float grams = (float)atof(cmd + 12);

        PRINTF("SYS,REMOTE_CMD_RX,TWIN_WEIGHT,%.2f\r\n", grams);

        if (Weight_SetTwinOverride(grams))
        {
            PRINTF("SYS,REMOTE_CMD_DONE,TWIN_WEIGHT,APPLIED,%.2f\r\n", grams);
            return true;
        }
        else
        {
            PRINTF("SYS,REMOTE_CMD_IGNORED,TWIN_WEIGHT\r\n");
            return false;
        }
    }

    if (strcmp(cmd, "DISPENSE") == 0)
    {
        DispenseVerifyResult result;
        DoseEvent evt;
        const char *med_id = "MANUAL";

        PRINTF("SYS,REMOTE_CMD_RX,DISPENSE\r\n");

        if (Reminder_GetActiveDoseEvent(&evt))
        {
            med_id = evt.med_id;
        }

        if (DispenseVerify_Run(med_id, &result))
        {
            PRINTF("SYS,REMOTE_CMD_DONE,DISPENSE,%s\r\n", result.result);
            return true;
        }
        else
        {
            PRINTF("SYS,REMOTE_CMD_DONE,DISPENSE,%s\r\n", result.result);
            return false;
        }
    }

    PRINTF("SYS,REMOTE_CMD_UNKNOWN,%s\r\n", cmd);
    return false;
}









