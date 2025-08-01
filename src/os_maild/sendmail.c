/* Copyright (C) 2015, Wazuh Inc.
 * Copyright (C) 2009 Trend Micro Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

/* Basic e-mailing operations */

#include "shared.h"
#include "os_net/os_net.h"
#include "maild.h"
#include "mail_list.h"

/* Return codes (from SMTP server) */
#define VALIDBANNER     "220"
#define VALIDMAIL       "250"
#define VALIDDATA       "354"

/* Default values used to connect */
#define SMTP_DEFAULT_PORT   25
#define MAILFROM            "Mail From: <%s>\r\n"
#define RCPTTO              "Rcpt To: <%s>\r\n"
#define DATAMSG             "DATA\r\n"
#define FROM                "From: " __ossec_name " <%s>\r\n"
#define TO                  "To: %s\r\n"
#define REPLYTO             "Reply-To: " __ossec_name " <%s>\r\n"
/*#define CC                "Cc: <%s>\r\n"*/
#define SUBJECT             "Subject: %s\r\n"
#define ENDHEADER           "\r\n"
#define ENDDATA             "\r\n.\r\n"
#define QUITMSG             "QUIT\r\n"
#define XHEADER             "X-IDS-OSSEC: %s\r\n"

/* Error messages - Can be translated */
#define INTERNAL_ERROR  "(1760): Memory/configuration error"
#define BANNER_ERROR    "(1762): Banner not received from server"
#define HELO_ERROR      "(1763): Hello not accepted by server"
#define FROM_ERROR      "(1764): Mail from not accepted by server"
#define TO_ERROR        "(1765): RCPT TO not accepted by server - '%s'."
#define DATA_ERROR      "(1766): DATA not accepted by server"
#define END_DATA_ERROR  "(1767): End of DATA not accepted by server"

#define MAIL_DEBUG_FLAG     0
#define MAIL_DEBUG(x,y,z) if(MAIL_DEBUG_FLAG) merror(x,y,z)

static bool append_mailbox(char **pp, size_t *prem, bool *pfirst, const char *addr) {
    char *p   = *pp;
    size_t rem = *prem;
    int n;

    // If it is not the first, the comma and the space are placed in front.
    if (!*pfirst) {
        n = snprintf(p, rem, ", ");
        if (n < 0 || (size_t)n >= rem) return false;
        p   += n;
        rem -= n;
    }

    // The address is added between <…>
    n = snprintf(p, rem, "<%s>", addr);
    if (n < 0 || (size_t)n >= rem) return false;
    p   += n;
    rem -= n;

    *pp     = p;
    *prem   = rem;
    *pfirst = false;
    return true;
}

int OS_Sendsms(MailConfig *mail, struct tm *p, MailMsg *sms_msg)
{
    wfd_t *sendmail = NULL;
    int socket = -1;
    size_t final_to_sz;
    char *msg;
    char snd_msg[128];
    char final_to[512];

    if (mail->smtpserver[0] == '/') {
        char *exec_cmd[2] = { mail->smtpserver, NULL };
        sendmail = wpopenv(mail->smtpserver, exec_cmd, W_BIND_STDIN);
        if (!sendmail) {
            return (OS_INVALID);
        }
    } else {
        /* Connect to the SMTP server */
        socket = OS_ConnectTCP(SMTP_DEFAULT_PORT, mail->smtpserver, 0, 0);
        if (socket < 0) {
            return (socket);
        }

        if (OS_SetRecvTimeout(socket, SOCK_RECV_TIME0, 0) < 0) {
            merror("Couldn't set receiving timeout for socket.");
        }


        /* Receive the banner */
        msg = OS_RecvTCP(socket, OS_SIZE_1024);
        if ((msg == NULL) || (!OS_Match(VALIDBANNER, msg))) {
            merror(BANNER_ERROR);
            if (msg) {
                free(msg);
            }
            close(socket);
            return (OS_INVALID);
        }
        MAIL_DEBUG("DEBUG: Received banner: '%s' %s", msg, "");
        free(msg);

        /* Send HELO message */
        memset(snd_msg, '\0', 128);
        if (mail->heloserver) {
            snprintf(snd_msg, 127, "Helo %s\r\n", mail->heloserver);
        } else {
            snprintf(snd_msg, 127, "Helo %s\r\n", "notify.ossec.net");
        }
        OS_SendTCP(socket, snd_msg);
        msg = OS_RecvTCP(socket, OS_SIZE_1024);
        if ((msg == NULL) || (!OS_Match(VALIDMAIL, msg))) {
            if (msg) {
                /* In some cases (with virus scans in the middle)
                 * we may get two banners. Check for that in here.
                 */
                if (OS_Match(VALIDBANNER, msg)) {
                    free(msg);

                    /* Try again */
                    msg = OS_RecvTCP(socket, OS_SIZE_1024);
                    if ((msg == NULL) || (!OS_Match(VALIDMAIL, msg))) {
                        merror("%s:%s", HELO_ERROR, msg != NULL ? msg : "null");
                        if (msg) {
                            free(msg);
                        }
                        close(socket);
                        return (OS_INVALID);
                    }
                } else {
                    merror("%s:%s", HELO_ERROR, msg);
                    free(msg);
                    close(socket);
                    return (OS_INVALID);
                }
            } else {
                merror("%s:%s", HELO_ERROR, "null");
                close(socket);
                return (OS_INVALID);
            }
        }

        MAIL_DEBUG("DEBUG: Sent '%s', received: '%s'", snd_msg, msg);
        free(msg);

        /* Build "Mail from" msg */
        memset(snd_msg, '\0', 128);
        snprintf(snd_msg, 127, MAILFROM, mail->from);
        OS_SendTCP(socket, snd_msg);
        msg = OS_RecvTCP(socket, OS_SIZE_1024);
        if ((msg == NULL) || (!OS_Match(VALIDMAIL, msg))) {
            merror(FROM_ERROR);
            if (msg) {
                free(msg);
            }
            close(socket);
            return (OS_INVALID);
        }
        MAIL_DEBUG("DEBUG: Sent '%s', received: '%s'", snd_msg, msg);
        free(msg);

        /* Additional RCPT to */
        final_to[0] = '\0';
        final_to_sz = sizeof(final_to) - 2;

        if (mail->gran_to) {
            int i = 0;
            bool first_sms = true;

            while (mail->gran_to[i] != NULL) {
                if (mail->gran_set[i] != SMS_FORMAT) {
                    i++;
                    continue;
                }

                /* Send RCPT TO: and validate */
                memset(snd_msg, '\0', 128);
                snprintf(snd_msg, 127, RCPTTO, mail->gran_to[i]);
                OS_SendTCP(socket, snd_msg);
                msg = OS_RecvTCP(socket, OS_SIZE_1024);
                if (!msg || !OS_Match(VALIDMAIL, msg)) {
                    merror(TO_ERROR, mail->gran_to[i]);
                    free(msg);
                    close(socket);
                    return OS_INVALID;
                }
                MAIL_DEBUG("DEBUG: Sent '%s', received: '%s'", snd_msg, msg);
                free(msg);

                /* Accumulate in final_to with snprintf */
                size_t len = strlen(final_to);
                if (!first_sms) {
                    int n = snprintf(final_to + len, final_to_sz, ", <%s>", mail->gran_to[i]);
                    if (n > 0) final_to_sz -= n;
                } else {
                    int n = snprintf(final_to + len, final_to_sz, "<%s>", mail->gran_to[i]);
                    if (n > 0) {
                        final_to_sz -= n;
                        first_sms = false;
                    }
                }

                i++;
            }
        }

        /* Send the "DATA" msg */
        OS_SendTCP(socket, DATAMSG);
        msg = OS_RecvTCP(socket, OS_SIZE_1024);
        if ((msg == NULL) || (!OS_Match(VALIDDATA, msg))) {
            merror(DATA_ERROR);
            if (msg) {
                free(msg);
            }

            close(socket);
            return (OS_INVALID);

        }
        MAIL_DEBUG("DEBUG: Sent '%s', received: '%s'", DATAMSG, msg);
        free(msg);

        /* Send a single To: with the entire list */
        if (final_to[0]) {
            memset(snd_msg, 0, sizeof(snd_msg));
            int n = snprintf(snd_msg, sizeof(snd_msg) - 1, TO, final_to);
            if (n < 0 || n >= (int)(sizeof(snd_msg)-1)) {
                merror("Error formatting 'To' header: snprintf failed or output truncated.");
                return OS_INVALID;
            }
            OS_SendTCP(socket, snd_msg);
        }
    }

    memset(snd_msg, '\0', 128);
    snprintf(snd_msg, 127, FROM, mail->from);

    if (sendmail) {
        fprintf(sendmail->file_in, "%s", snd_msg);
    } else {
        OS_SendTCP(socket, snd_msg);
    }

    /* Send reply-to if set */
    if (mail->reply_to){
        memset(snd_msg, '\0', 128);
        snprintf(snd_msg, 127, REPLYTO, mail->reply_to);

        if (sendmail) {
            fprintf(sendmail->file_in, "%s", snd_msg);
        } else {
            OS_SendTCP(socket, snd_msg);
        }
    }

    /* Send date */
    memset(snd_msg, '\0', 128);

    /* Solaris doesn't have the "%z", so we set the timezone to 0 */
#ifdef SOLARIS
    strftime(snd_msg, 127, "Date: %a, %d %b %Y %T -0000\r\n", p);
#else
    strftime(snd_msg, 127, "Date: %a, %d %b %Y %T %z\r\n", p);
#endif

    if (sendmail) {
        fprintf(sendmail->file_in, "%s", snd_msg);
    } else {
        OS_SendTCP(socket, snd_msg);
    }

    /* Send subject */
    memset(snd_msg, '\0', 128);
    snprintf(snd_msg, 127, SUBJECT, sms_msg->subject);

    if (sendmail) {
        fprintf(sendmail->file_in, "%s", snd_msg);
        fprintf(sendmail->file_in, ENDHEADER);
        fprintf(sendmail->file_in, "%s", sms_msg->body);

        fflush(sendmail->file_in);
        if (wpclose(sendmail) == -1) {
            merror(WAITPID_ERROR, errno, strerror(errno));
        }
    } else {
        OS_SendTCP(socket, snd_msg);
        OS_SendTCP(socket, ENDHEADER);

        /* Send body */
        OS_SendTCP(socket, sms_msg->body);

        /* Send end of data \r\n.\r\n */
        OS_SendTCP(socket, ENDDATA);
        msg = OS_RecvTCP(socket, OS_SIZE_1024);
        if (mail->strict_checking && ((msg == NULL) || (!OS_Match(VALIDMAIL, msg)))) {
            merror(END_DATA_ERROR);
            if (msg) {
                free(msg);
            }
            close(socket);
            return (OS_INVALID);
        }
        /* Check msg, since it may be null */
        if (msg) {
            free(msg);
        }

        /* Quit and close socket */
        OS_SendTCP(socket, QUITMSG);
        msg = OS_RecvTCP(socket, OS_SIZE_1024);

        if (msg) {
            free(msg);
        }

        close(socket);
    }

    memset_secure(snd_msg, '\0', 128);
    return (0);
}

int OS_Sendmail(MailConfig *mail, struct tm *p)
{
    wfd_t *sendmail = NULL;
    int socket = -1;
    unsigned int i = 0;
    char *msg;
    char snd_msg[256];

    MailNode *mailmsg;

    /* If there is no sms message, attempt to get from the email list */
    mailmsg = OS_PopLastMail();

    if (mailmsg == NULL) {
        merror("No email to be sent. Inconsistent state.");
        return (OS_INVALID);
    }

    if (mail->smtpserver[0] == '/') {
        char *exec_cmd[2] = { mail->smtpserver, NULL };
        sendmail = wpopenv(mail->smtpserver, exec_cmd, W_BIND_STDIN);
        if (!sendmail) {
            return (OS_INVALID);
        }
    } else {
        /* Connect to the SMTP server */
        socket = OS_ConnectTCP(SMTP_DEFAULT_PORT, mail->smtpserver, 0, 0);
        if (socket < 0) {
            return (socket);
        }

        if (OS_SetRecvTimeout(socket, SOCK_RECV_TIME0, 0) < 0) {
            merror("Couldn't set receiving timeout for socket.");
        }

        /* Receive the banner */
        msg = OS_RecvTCP(socket, OS_SIZE_1024);
        if ((msg == NULL) || (!OS_Match(VALIDBANNER, msg))) {
            merror(BANNER_ERROR);
            if (msg) {
                free(msg);
            }
            close(socket);
            return (OS_INVALID);
        }
        MAIL_DEBUG("DEBUG: Received banner: '%s' %s", msg, "");
        free(msg);

        /* Send HELO message */
        memset(snd_msg, '\0', 256);
        if (mail->heloserver) {
            snprintf(snd_msg, 256, "Helo %s\r\n", mail->heloserver);
        } else {
            snprintf(snd_msg, 256, "Helo %s\r\n", "notify.ossec.net");
        }
        OS_SendTCP(socket, snd_msg);
        msg = OS_RecvTCP(socket, OS_SIZE_1024);
        if ((msg == NULL) || (!OS_Match(VALIDMAIL, msg))) {
            if (msg) {
                /* In some cases (with virus scans in the middle)
                 * we may get two banners. Check for that in here.
                 */
                if (OS_Match(VALIDBANNER, msg)) {
                    free(msg);

                    /* Try again */
                    msg = OS_RecvTCP(socket, OS_SIZE_1024);
                    if ((msg == NULL) || (!OS_Match(VALIDMAIL, msg))) {
                        merror("%s:%s", HELO_ERROR, msg != NULL ? msg : "null");
                        if (msg) {
                            free(msg);
                        }
                        close(socket);
                        return (OS_INVALID);
                    }
                } else {
                    merror("%s:%s", HELO_ERROR, msg);
                    free(msg);
                    close(socket);
                    return (OS_INVALID);
                }
            } else {
                merror("%s:%s", HELO_ERROR, "null");
                close(socket);
                return (OS_INVALID);
            }
        }

        MAIL_DEBUG("Sent '%s', received: '%s'", snd_msg, msg);
        free(msg);

        /* Build "Mail from" msg */
        memset(snd_msg, '\0', 256);
        snprintf(snd_msg, 256, MAILFROM, mail->from);
        OS_SendTCP(socket, snd_msg);
        msg = OS_RecvTCP(socket, OS_SIZE_1024);
        if ((msg == NULL) || (!OS_Match(VALIDMAIL, msg))) {
            merror(FROM_ERROR);
            if (msg) {
                free(msg);
            }
            close(socket);
            return (OS_INVALID);
        }
        MAIL_DEBUG("Sent '%s', received: '%s'", snd_msg, msg);
        free(msg);

        /* Build "RCPT TO" msg */
        while (1) {
            if (mail->to == NULL) break;
            if (mail->to[i] == NULL) {
                if (i == 0) {
                    merror(INTERNAL_ERROR);
                    close(socket);
                    return (OS_INVALID);
                }
                break;
            }
            memset(snd_msg, '\0', 256);
            snprintf(snd_msg, 256, RCPTTO, mail->to[i++]);
            OS_SendTCP(socket, snd_msg);
            msg = OS_RecvTCP(socket, OS_SIZE_1024);
            if ((msg == NULL) || (!OS_Match(VALIDMAIL, msg))) {
                merror(TO_ERROR, mail->to[i - 1]);
                if (msg) {
                    free(msg);
                }
                close(socket);
                return (OS_INVALID);
            }
            MAIL_DEBUG("DEBUG: Sent '%s', received: '%s'", snd_msg, msg);
            free(msg);
        }

        /* Additional RCPT to */
        if (mail->gran_to) {
            i = 0;
            while (mail->gran_to[i] != NULL) {
                if (mail->gran_set[i] != FULL_FORMAT) {
                    i++;
                    continue;
                }

                memset(snd_msg, '\0', 256);
                snprintf(snd_msg, 256, RCPTTO, mail->gran_to[i]);
                OS_SendTCP(socket, snd_msg);
                msg = OS_RecvTCP(socket, OS_SIZE_1024);
                if ((msg == NULL) || (!OS_Match(VALIDMAIL, msg))) {
                    merror(TO_ERROR, mail->gran_to[i]);
                    if (msg) {
                        free(msg);
                    }

                    i++;
                    continue;
                }

                MAIL_DEBUG("DEBUG: Sent '%s', received: '%s'", snd_msg, msg);
                free(msg);
                i++;
                continue;
            }
        }

        /* Send the "DATA" msg */
        OS_SendTCP(socket, DATAMSG);
        msg = OS_RecvTCP(socket, OS_SIZE_1024);
        if ((msg == NULL) || (!OS_Match(VALIDDATA, msg))) {
            merror(DATA_ERROR);
            if (msg) {
                free(msg);
            }
            close(socket);
            return (OS_INVALID);
        }
        MAIL_DEBUG("DEBUG: Sent '%s', received: '%s'", DATAMSG, msg);
        free(msg);
    }

    memset(snd_msg, '\0', 256);
    snprintf(snd_msg, 255, FROM, mail->from);

    if (sendmail) {
        fprintf(sendmail->file_in, "%s", snd_msg);
    } else {
        OS_SendTCP(socket, snd_msg);
    }

    /* Send reply-to if set */
    if (mail->reply_to){
        memset(snd_msg, '\0', 256);
        snprintf(snd_msg, 256, REPLYTO, mail->reply_to);
        if (sendmail) {
            fprintf(sendmail->file_in, "%s", snd_msg);
        } else {
            OS_SendTCP(socket, snd_msg);
        }
    }

    if (mail->to || mail->gran_to) {
        char to_list[1024];
        char *p      = to_list;
        size_t rem   = sizeof(to_list);
        bool first   = true;

        if (mail->to) {
            for (int i = 0; mail->to[i]; i++) {
                if (!append_mailbox(&p, &rem, &first, mail->to[i])) break;
            }
        }

        if (mail->gran_to) {
            for (int i = 0; mail->gran_to[i]; i++) {
                if (mail->gran_set[i] != FULL_FORMAT) continue;
                if (!append_mailbox(&p, &rem, &first, mail->gran_to[i])) break;
            }
        }

        /* Send single TO: if there is at least one */
        if (p != to_list) {
            if (sendmail) {
                fprintf(sendmail->file_in, TO, to_list);
            } else {
                char hdr[ sizeof(to_list) + sizeof(TO) ];
                int n = snprintf(hdr, sizeof(hdr) - 1, TO, to_list);
                if (n < 0 || n >= (int)(sizeof(snd_msg) - 1)) {
                    merror("Error formatting 'To' header: snprintf failed or output truncated.");
                    return OS_INVALID;
                }
                OS_SendTCP(socket, hdr);
            }
        }
    }

    /* Send date */
    memset(snd_msg, '\0', 256);

    /* Solaris doesn't have the "%z", so we set the timezone to 0 */
#ifdef SOLARIS
    strftime(snd_msg, 255, "Date: %a, %d %b %Y %T -0000\r\n", p);
#else
    strftime(snd_msg, 255, "Date: %a, %d %b %Y %T %z\r\n", p);
#endif

    if (sendmail) {
        fprintf(sendmail->file_in, "%s", snd_msg);
    } else {
        OS_SendTCP(socket, snd_msg);
    }

    if (mail->idsname) {
        /* Send server name header */
        memset(snd_msg, '\0', 256);
        snprintf(snd_msg, 256, XHEADER, mail->idsname);

        if (sendmail) {
            fprintf(sendmail->file_in, "%s", snd_msg);
        } else {
            OS_SendTCP(socket, snd_msg);
        }
    }

    /* Send subject */
    memset(snd_msg, '\0', 256);

    /* Check if global subject is available */
    if ((_g_subject_level != 0) && (_g_subject[0] != '\0')) {
        snprintf(snd_msg, 256, SUBJECT, _g_subject);

        /* Clear global values */
        _g_subject[0] = '\0';
        _g_subject_level = 0;
    } else {
        snprintf(snd_msg, 256, SUBJECT, mailmsg->mail->subject);
    }

    if (sendmail) {
        fprintf(sendmail->file_in, "%s", snd_msg);
        fprintf(sendmail->file_in, ENDHEADER);
    } else {
        OS_SendTCP(socket, snd_msg);
        OS_SendTCP(socket, ENDHEADER);
    }

    /* Send body */

    /* Send multiple emails together if we have to */
    do {
        if (sendmail) {
            fprintf(sendmail->file_in, "%s", mailmsg->mail->body);
        } else {
            OS_SendTCP(socket, mailmsg->mail->body);
        }
        mailmsg = OS_PopLastMail();
    } while (mailmsg);

    if (sendmail) {
        fflush(sendmail->file_in);
        if (wpclose(sendmail) == -1) {
            merror(WAITPID_ERROR,errno, strerror(errno));
        }
    } else {
        /* Send end of data \r\n.\r\n */
        OS_SendTCP(socket, ENDDATA);
        msg = OS_RecvTCP(socket, OS_SIZE_1024);
        if (mail->strict_checking && ((msg == NULL) || (!OS_Match(VALIDMAIL, msg)))) {
            merror(END_DATA_ERROR);
            if (msg) {
                free(msg);
            }
            close(socket);
            return (OS_INVALID);
        }

        /* Check msg, since it may be null */
        if (msg) {
            free(msg);
        }

        /* Quit and close socket */
        OS_SendTCP(socket, QUITMSG);
        msg = OS_RecvTCP(socket, OS_SIZE_1024);

        if (msg) {
            free(msg);
        }

        close(socket);
    }

    memset_secure(snd_msg, '\0', 256);
    return (0);
}
