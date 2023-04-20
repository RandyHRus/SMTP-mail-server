#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024

typedef enum state
{
    Init,
    Executed_Helo,
    Mail_transaction_open,
    Recipient_provided,
    Data_input,
    Data_input_done
} State;

typedef struct smtp_state
{
    int fd;
    net_buffer_t nb;
    char recvbuf[MAX_LINE_LENGTH + 1];
    char *words[MAX_LINE_LENGTH];
    int nwords;
    State state;
    struct utsname my_uname;
    user_list_t reverse_path_buffer;
    user_list_t forward_path_buffer;
    char *mail_data_buffer;
} smtp_state;

// https://www.rfc-editor.org/rfc/rfc5321

static void handle_client(int fd);

int main(int argc, char *argv[])
{

    if (argc != 2)
    {
        fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
        return 1;
    }

    run_server(argv[1], handle_client);

    return 0;
}

// Resets
void clear_buffers(smtp_state *ms)
{
    if (ms->reverse_path_buffer)
    {
        user_list_destroy(ms->reverse_path_buffer);
        ms->reverse_path_buffer = NULL;
    }
    if (ms->forward_path_buffer)
    {
        user_list_destroy(ms->forward_path_buffer);
        ms->forward_path_buffer = NULL;
    }
    if (ms->mail_data_buffer)
    {
        free(ms->mail_data_buffer);
        ms->mail_data_buffer = NULL;
    }
}

// syntax_error returns
//   -1 if the server should exit
//    1  otherwise
int syntax_error(smtp_state *ms)
{
    if (send_formatted(ms->fd, "501 %s\r\n", "Syntax error in parameters or arguments") <= 0)
        return -1;
    return 1;
}

// checkstate returns
//   -1 if the server should exit
//    0 if the server is in the appropriate state
//    1 if the server is not in the appropriate state
int checkstate(smtp_state *ms, State s)
{
    if (ms->state != s)
    {
        if (send_formatted(ms->fd, "503 %s\r\n", "Bad sequence of commands") <= 0)
            return -1;
        return 1;
    }
    return 0;
}

// All the functions that implement a single command return
//   -1 if the server should exit
//    0 if the command was successful
//    1 if the command was unsuccessful

// receiver must send a 221 OK then close the transmission protocol.
int do_quit(smtp_state *ms)
{
    dlog("Executing quit\n");

    send_formatted(ms->fd, "221 Service closing transmission channel.\r\n");

    return -1;
}

/**
 *   Syntax: HELO SP Domain CRLF
 *   Used to identify the SMTP client to the SMTP server.
 *
 *   There must be no transaction in progress and all state tables and buffers are cleared.
 */
int do_helo(smtp_state *ms)
{
    dlog("Executing helo\n");

    if (ms->nwords != 2)
    {
        send_formatted(ms->fd, "501 Syntax error in arguments\r\n");
        return 1;
    }

    if (ms->state != Init)
    {
        dlog("not in right state\n");
        send_formatted(ms->fd, "503 Wrong sequence of commands\r\n");
        return 1;
    }

    dlog("Syntax OK\n");

    ms->state = Executed_Helo;
    send_formatted(ms->fd, "250 %s\r\n", ms->my_uname.nodename);

    ms->reverse_path_buffer = NULL;
    ms->forward_path_buffer = NULL;
    ms->mail_data_buffer = NULL;

    return 0;
}

/**
   This command specifies that the current mail transaction will be
   aborted.
   1. Any stored sender, recipients, and mail data MUST be
   discarded,
   2. All buffers and state tables cleared.
   3. The receiver MUST send a "250 OK" reply to a RSET command with no
   arguments.  A reset command may be issued by the client at any time.

   It is effectively equivalent to a NOOP (i.e., it has no effect) if issued
   immediately after EHLO, before EHLO is issued in the session, after
   an end of data indicator has been sent and acknowledged, or
   immediately before a QUIT.  An SMTP server MUST NOT close the
   connection as the result of receiving a RSET; that action is reserved
   for QUIT
*/
int do_rset(smtp_state *ms)
{
    dlog("Executing rset\n");

    if (ms->nwords != 1)
    {
        send_formatted(ms->fd, "501 Syntax error in arguments\r\n");
        return 1;
    }

    dlog("Syntax OK\n");

    if (ms->state != Init)
    {
        clear_buffers(ms);
        ms->state = Executed_Helo;
    }
    send_formatted(ms->fd, "250 State reset\r\n");

    return 1;
}

/**
 * Mail data is delivered
 * to one or more mailboxes or another system.
 *
 * command may only be executed after HELO.
 * command may only be sent when no mail transaction is in progress.
 *
 * This command clears the reverse-path buffer, forward-path buffer, and the mail data buffer.
 * It inserts the reverse-path information into reverse-path buffer.
 *
 * @param ms->words format: "MAIL FROM:" Reverse-path
 */
int do_mail(smtp_state *ms)
{
    dlog("Executing mail\n");

    if (ms->nwords != 2)
    {
        dlog("Syntax error\n");
        syntax_error(ms);
        return 1;
    }
    else if (strncasecmp(ms->words[1], "FROM:<", 6) != 0 || ms->words[1][strlen(ms->words[1]) - 1] != '>')
    {
        dlog("Syntax error\n");
        syntax_error(ms);
        return 1;
    }

    if (!(ms->state == Executed_Helo || ms->state == Data_input_done))
    {
        dlog("not in right state\n");
        send_formatted(ms->fd, "503 Wrong sequence of commands\r\n");
        return 1;
    }

    dlog("Syntax OK\n");

    int strlength = strlen(ms->words[1]);
    strlength = strlength - 7; // trim from:< and >
    char reverse_path[strlength + 1];
    strncpy(reverse_path, ms->words[1] + 6, strlength);
    reverse_path[strlength] = 0; // null terminating

    dlog("Got reverse path %s\n", reverse_path);

    clear_buffers(ms);
    dlog("Successfully cleared buffers\n");

    // insert the reverse-path information into reverse-path buffer.
    if (!ms->reverse_path_buffer)
    {
        ms->reverse_path_buffer = user_list_create();
    }
    user_list_add(&ms->reverse_path_buffer, reverse_path);

    dlog("Successfully added reverse-path info to buffer\n");

    send_formatted(ms->fd, "250 ok (mail)\r\n");

    ms->state = Mail_transaction_open;

    return 0;
}

/**
 * Used to identify an individual recipient of the mail data.
 *
 * Forward-path is destination mailbox.
 * Appends forward-path argument to forward-path buffer.
 *
 * Command may only be executed after MAIL.
 *
 * @param ms->words format: "RCPT TO:" ( "<Postmaster@" Domain ">" / "<Postmaster>" / Forward-path ) [SP Rcpt-parameters]
 */
int do_rcpt(smtp_state *ms)
{
    dlog("Executing rcpt\n");

    if (ms->nwords != 2)
    {
        dlog("Syntax error\n");
        syntax_error(ms);
        return 1;
    }
    else if (strncasecmp(ms->words[1], "TO:<", 4) != 0 || ms->words[1][strlen(ms->words[1]) - 1] != '>')
    {
        dlog("Syntax error\n");
        syntax_error(ms);
        return 1;
    }

    if (!(ms->state == Mail_transaction_open || ms->state == Recipient_provided))
    {
        dlog("not in right state\n");
        send_formatted(ms->fd, "503 Wrong sequence of commands\r\n");
        return 1;
    }

    dlog("Syntax OK\n");

    int strlength = strlen(ms->words[1]);
    strlength = strlength - 5; // trim to:< and >
    char forward_path[strlength + 1];
    strncpy(forward_path, ms->words[1] + 4, strlength);
    forward_path[strlength] = 0; // null terminating

    dlog("Got forward path %s with length %d\n", forward_path, strlength);

    if (is_valid_user(forward_path, NULL))
    {
        if (!ms->forward_path_buffer)
        {
            ms->forward_path_buffer = user_list_create();
        }
        // Appends forward-path argument to forward-path buffer.
        user_list_add(&ms->forward_path_buffer, forward_path);
        ms->state = Recipient_provided;
        send_formatted(ms->fd, "250 OK (rcpt)\r\n");
        return 0;
    }
    else
    {
        send_formatted(ms->fd, "550 No such user - %s\r\n", forward_path);
        return 1;
    }
}

/**
 * Mail data gets appended to the mail data buffer.
 *
 * @param ms->words format: "DATA"
 *
 * data will be accepted in multiple lines, ending with ending with indication <CRLF>.<CRLF>.
 *
 *  At the end, process information in the reverse-path buffer, the forward-path
 *  buffer, and the mail data buffer, then buffers are cleared.
 */
int do_data(smtp_state *ms)
{
    dlog("Executing data\n");

    if (ms->nwords != 1)
    {
        dlog("Syntax error\n");
        syntax_error(ms);
        return 1;
    }

    if (ms->state != Recipient_provided)
    {
        dlog("not in right state\n");
        send_formatted(ms->fd, "503 Wrong sequence of commands\r\n");
        return 1;
    }

    dlog("Syntax OK\n");

    ms->state = Data_input;

    send_formatted(ms->fd, "354 Start mail input; end with <CRLF>.<CRLF>\r\n");

    size_t len;

    while ((len = nb_read_line(ms->nb, ms->recvbuf)) >= 0)
    {
        if (len == 0)
        {
            continue;
        }

        // Remove CR, LF and other space characters from end of buffer
        while (isspace(ms->recvbuf[len - 1]))
            ms->recvbuf[--len] = 0;

        dlog("received data: %s, length %zu\n", ms->recvbuf, len);

        // End of data
        if (strcmp(ms->recvbuf, ".") == 0)
        {
            dlog("Saving user mail\n");

            // Create temporary file
            char fileName[] = "maildata_tmpXXXXXX";
            int file = mkstemp(fileName);

            // don't write anything if mail content is empty.
            if (ms->mail_data_buffer != NULL)
            {
                write(file, ms->mail_data_buffer, strlen(ms->mail_data_buffer));
            }

            save_user_mail(fileName, ms->forward_path_buffer);
            close(file);
            remove(fileName); // delete temp file

            clear_buffers(ms);

            ms->state = Data_input_done;
            send_formatted(ms->fd, "250 OK data done\r\n");

            return 0;
        }

        dlog("string to write: %s, length %zu\n", ms->recvbuf, len);

        if (ms->mail_data_buffer == NULL)
        {
            ms->mail_data_buffer = malloc(len + 1);
            ms->mail_data_buffer[0] = 0;
            dlog("strcat: %s \n", ms->recvbuf);
            strcat(ms->mail_data_buffer, ms->recvbuf);
            ms->mail_data_buffer[len] = 0;
        }
        else
        {
            // If first character is . we need to remove it
            if (ms->recvbuf[0] == '.')
            {
                ms->mail_data_buffer = realloc(ms->mail_data_buffer, strlen(ms->mail_data_buffer) + len - 1 + 1);
                dlog("strcat: %s \n", ms->recvbuf + 1);
                strcat(ms->mail_data_buffer, ms->recvbuf + 1);
                ms->mail_data_buffer[strlen(ms->mail_data_buffer)] = 0;
            }
            else
            {
                ms->mail_data_buffer = realloc(ms->mail_data_buffer, strlen(ms->mail_data_buffer) + len + 1);
                dlog("strcat: %s \n", ms->recvbuf);
                strcat(ms->mail_data_buffer, ms->recvbuf);
                ms->mail_data_buffer[strlen(ms->mail_data_buffer)] = 0;
            }
        }

        // Add <CRLF>
        ms->mail_data_buffer = realloc(ms->mail_data_buffer, strlen(ms->mail_data_buffer) + 2 + 1);
        strcat(ms->mail_data_buffer, "\r\n");
        ms->mail_data_buffer[strlen(ms->mail_data_buffer)] = 0;
        dlog("new buffer size %lu \n", strlen(ms->mail_data_buffer));
    }

    send_formatted(ms->fd, "501\n");
    return 1;
}

// receiver must send a 250 OK replay
int do_noop(smtp_state *ms)
{
    dlog("Executing noop\n");
    send_formatted(ms->fd, "250 OK (noop)\r\n");
    return 0;
}

/**
 * Confirm that the argument identifies a user or mailbox.
 * If it is a user name, information is returned.
 */
int do_vrfy(smtp_state *ms)
{
    dlog("Executing vrfy\n");

    if (ms->nwords != 2)
    {
        syntax_error(ms);
        return 1;
    }

    // if username exists return non-zero (true),
    // zero (false) otherwise.
    if (is_valid_user(ms->words[1], NULL))
    {
        send_formatted(ms->fd, "250 <%s>\r\n", ms->words[1]);
        return 0;
    }
    else
    {
        send_formatted(ms->fd, "550 No such user - %s\r\n", ms->words[1]);
        return 0;
    }
}

void handle_client(int fd)
{

    size_t len;
    smtp_state mstate, *ms = &mstate;

    ms->fd = fd;
    ms->nb = nb_create(fd, MAX_LINE_LENGTH);
    ms->state = Init;
    uname(&ms->my_uname);

    if (send_formatted(fd, "220 %s Service ready\r\n", ms->my_uname.nodename) <= 0)
        return;

    while ((len = nb_read_line(ms->nb, ms->recvbuf)) >= 0)
    {

        if (ms->recvbuf[len - 1] != '\n')
        {
            // command line is too long, stop immediately
            send_formatted(fd, "500 Syntax error, command unrecognized\r\n");
            break;
        }
        if (strlen(ms->recvbuf) < len)
        {
            // received null byte somewhere in the string, stop immediately.
            send_formatted(fd, "500 Syntax error, command unrecognized\r\n");
            break;
        }

        // Remove CR, LF and other space characters from end of buffer
        while (isspace(ms->recvbuf[len - 1]))
            ms->recvbuf[--len] = 0;

        dlog("Command is %s\n", ms->recvbuf);

        // Split the command into its component "words"
        ms->nwords = split(ms->recvbuf, ms->words);
        char *command = ms->words[0];

        if (!strcasecmp(command, "QUIT"))
        {
            if (do_quit(ms) == -1)
                break;
        }
        else if (!strcasecmp(command, "HELO") || !strcasecmp(command, "EHLO"))
        {
            if (do_helo(ms) == -1)
                break;
        }
        else if (!strcasecmp(command, "MAIL"))
        {
            if (do_mail(ms) == -1)
                break;
        }
        else if (!strcasecmp(command, "RCPT"))
        {
            if (do_rcpt(ms) == -1)
                break;
        }
        else if (!strcasecmp(command, "DATA"))
        {
            if (do_data(ms) == -1)
                break;
        }
        else if (!strcasecmp(command, "RSET"))
        {
            if (do_rset(ms) == -1)
                break;
        }
        else if (!strcasecmp(command, "NOOP"))
        {
            if (do_noop(ms) == -1)
                break;
        }
        else if (!strcasecmp(command, "VRFY"))
        {
            if (do_vrfy(ms) == -1)
                break;
        }
        else if (!strcasecmp(command, "EXPN") ||
                 !strcasecmp(command, "HELP"))
        {
            dlog("Command not implemented \"%s\"\n", command);
            if (send_formatted(fd, "502 Command not implemented\r\n") <= 0)
                break;
        }
        else
        {
            // invalid command
            dlog("Illegal command \"%s\"\n", command);
            if (send_formatted(fd, "500 Syntax error, command unrecognized\r\n") <= 0)
                break;
        }
    }

    nb_destroy(ms->nb);
}
