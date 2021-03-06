/* Copyright (C) 2009 Trend Micro Inc.
 * All right reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

/* Rootcheck decoder */

#include "config.h"
#include "os_regex/os_regex.h"
#include "eventinfo.h"
#include "alerts/alerts.h"
#include "decoder.h"
#include "rootcheck_op.h"

#define ROOTCHECK_DIR    "/queue/rootcheck"

/* Local variables */
static char *rk_agent_ips[MAX_AGENTS];
static FILE *rk_agent_fps[MAX_AGENTS];
static int rk_err;

/* Rootcheck decoder */
static OSDecoderInfo *rootcheck_dec = NULL;

/* Initialize the necessary information to process the rootcheck information */
void RootcheckInit()
{
    int i = 0;

    rk_err = 0;

    for (; i < MAX_AGENTS; i++) {
        rk_agent_ips[i] = NULL;
        rk_agent_fps[i] = NULL;
    }

    /* Zero decoder */
    os_calloc(1, sizeof(OSDecoderInfo), rootcheck_dec);
    rootcheck_dec->id = getDecoderfromlist(ROOTCHECK_MOD);
    rootcheck_dec->type = OSSEC_RL;
    rootcheck_dec->name = ROOTCHECK_MOD;
    rootcheck_dec->fts = 0;

    /* New fields as dynamic */

    os_calloc(Config.decoder_order_size, sizeof(char *), rootcheck_dec->fields);
    rootcheck_dec->fields[RK_TITLE] = "title";
    rootcheck_dec->fields[RK_FILE] = "file";

    mdebug1("RootcheckInit completed.");

    return;
}

/* Return the file pointer to be used */
static FILE *RK_File(const char *agent, int *agent_id)
{
    int i;
    char rk_buf[OS_SIZE_1024 + 1];

    for (i = 0; rk_agent_ips[i]; i++) {
        if (strcmp(rk_agent_ips[i], agent) == 0) {
            snprintf(rk_buf, OS_SIZE_1024, "%s/%s", ROOTCHECK_DIR, agent);

            if (!IsFile(rk_buf)) {
                /* Pointing to the beginning of the file */
                fseek(rk_agent_fps[i], 0, SEEK_SET);
                *agent_id = i;
                return (rk_agent_fps[i]);
            } else {
                // File was deleted. Close and let reopen.
                mwarn("Rootcheck database '%s' has been deleted. Recreating.", agent);
                fclose(rk_agent_fps[i]);
                free(rk_agent_ips[i]);
                rk_agent_ips[i] = NULL;
                break;
            }
        }
    }

    /* If here, our agent wasn't found */
    rk_agent_ips[i] = strdup(agent);

    if (rk_agent_ips[i] != NULL) {
        snprintf(rk_buf, OS_SIZE_1024, "%s/%s", ROOTCHECK_DIR, agent);

        /* r+ to read and write. Do not truncate */
        rk_agent_fps[i] = fopen(rk_buf, "r+");
        if (!rk_agent_fps[i]) {
            /* Try opening with a w flag, file probably does not exist */
            rk_agent_fps[i] = fopen(rk_buf, "w");
            if (rk_agent_fps[i]) {
                fclose(rk_agent_fps[i]);
                rk_agent_fps[i] = fopen(rk_buf, "r+");
            }
        }
        if (!rk_agent_fps[i]) {
            merror(FOPEN_ERROR, rk_buf, errno, strerror(errno));

            free(rk_agent_ips[i]);
            rk_agent_ips[i] = NULL;

            return (NULL);
        }

        /* Return the opened pointer (the beginning of it) */
        fseek(rk_agent_fps[i], 0, SEEK_SET);
        *agent_id = i;
        return (rk_agent_fps[i]);
    }

    else {
        merror(MEM_ERROR, errno, strerror(errno));
        return (NULL);
    }

    return (NULL);
}

/* Special decoder for rootcheck
 * Not using the default rendering tools for simplicity
 * and to be less resource intensive
 */
int DecodeRootcheck(Eventinfo *lf)
{
    int agent_id;

    char *tmpstr;
    char rk_buf[OS_SIZE_2048 + 1];

    FILE *fp;

    fpos_t fp_pos;

    /* Zero rk_buf */
    rk_buf[0] = '\0';
    rk_buf[OS_SIZE_2048] = '\0';

    fp = RK_File(lf->location, &agent_id);

    if (!fp) {
        merror("Error handling rootcheck database.");
        rk_err++;

        return (0);
    }

    /* Get initial position */
    if (fgetpos(fp, &fp_pos) == -1) {
        merror("Error handling rootcheck database (fgetpos).");
        return (0);
    }


    /* Reads the file and search for a possible entry */
    while (fgets(rk_buf, OS_SIZE_2048 - 1, fp) != NULL) {
        /* Ignore blank lines and lines with a comment */
        if (rk_buf[0] == '\n' || rk_buf[0] == '#') {
            if (fgetpos(fp, &fp_pos) == -1) {
                merror("Error handling rootcheck database (fgetpos2).");
                return (0);
            }
            continue;
        }

        /* Remove newline */
        tmpstr = strchr(rk_buf, '\n');
        if (tmpstr) {
            *tmpstr = '\0';
        }

        /* Old format without the time stamps */
        if (rk_buf[0] != '!') {
            /* Cannot use strncmp to avoid errors with crafted files */
            if (strcmp(lf->log, rk_buf) == 0) {
                rootcheck_dec->fts = 0;
                lf->decoder_info = rootcheck_dec;
                lf->nfields = RK_NFIELDS;
                lf->fields[RK_TITLE].key = rootcheck_dec->fields[RK_TITLE];
                lf->fields[RK_TITLE].value = rk_get_title(lf->log);
                lf->fields[RK_FILE].key = rootcheck_dec->fields[RK_FILE];
                lf->fields[RK_FILE].value = rk_get_file(lf->log);
                return (1);
            }
        }
        /* New format */
        else {
            /* Going past time: !1183431603!1183431603  (last, first seen) */
            tmpstr = rk_buf + 23;

            /* Matches, we need to upgrade last time saw */
            if (strcmp(lf->log, tmpstr) == 0) {
                if(fsetpos(fp, &fp_pos)) {
                    merror("Error handling rootcheck database (fsetpos).");
                    return (0);
                }
                fprintf(fp, "!%ld", (long int)lf->time);
                fflush(fp);
                rootcheck_dec->fts = 0;
                lf->decoder_info = rootcheck_dec;
                lf->nfields = RK_NFIELDS;
                lf->fields[RK_TITLE].key = rootcheck_dec->fields[RK_TITLE];
                lf->fields[RK_TITLE].value = rk_get_title(lf->log);
                lf->fields[RK_FILE].key = rootcheck_dec->fields[RK_FILE];
                lf->fields[RK_FILE].value = rk_get_file(lf->log);
                return (1);
            }
        }

        /* Get current position */
        if (fgetpos(fp, &fp_pos) == -1) {
            merror("Error handling rootcheck database (fgetpos3).");
            return (0);
        }
    }

    /* Add the new entry at the end of the file */
    fseek(fp, 0, SEEK_END);
    fprintf(fp, "!%ld!%ld %s\n", (long int)lf->time, (long int)lf->time, lf->log);
    fflush(fp);

    rootcheck_dec->fts = FTS_DONE;
    lf->decoder_info = rootcheck_dec;
    lf->nfields = RK_NFIELDS;
    lf->fields[RK_TITLE].key = rootcheck_dec->fields[RK_TITLE];
    lf->fields[RK_TITLE].value = rk_get_title(lf->log);
    lf->fields[RK_FILE].key = rootcheck_dec->fields[RK_FILE];
    lf->fields[RK_FILE].value = rk_get_file(lf->log);
    return (1);
}
