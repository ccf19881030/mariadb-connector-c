/************************************************************************************
   Copyright (C) 2015 MariaDB Corporation AB
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.
   
   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.
   
   You should have received a copy of the GNU Library General Public
   License along with this library; if not see <http://www.gnu.org/licenses>
   or write to the Free Software Foundation, Inc., 
   51 Franklin St., Fifth Floor, Boston, MA 02110, USA
*************************************************************************************/
#ifndef _WIN32
#define _GNU_SOURCE 1
#endif

#include <my_global.h>
#include <mysql.h>
#include <mysql/client_plugin.h>
#include <string.h>
#include <memory.h>

#ifndef WIN32
#include <dlfcn.h>
#endif

#define READ  0
#define WRITE 1

/* function prototypes */
static int trace_init(char *errormsg, 
                      size_t errormsg_size,
                      int unused      __attribute__((unused)), 
                      va_list unused1 __attribute__((unused)));
static int trace_deinit(void);

int (*register_callback)(my_bool register_callback, 
                         void (*callback_function)(int mode, MYSQL *mysql, const uchar *buffer, size_t length));
void trace_callback(int mode, MYSQL *mysql, const uchar *buffer, size_t length);

#ifndef HAVE_TRACE_EXAMPLE_PLUGIN_DYNAMIC
struct st_mysql_client_plugin trace_example_plugin=
#else
struct st_mysql_client_plugin _mysql_client_plugin_declaration_ =
#endif
{
  MARIADB_CLIENT_TRACE_PLUGIN,
  MARIADB_CLIENT_TRACE_PLUGIN_INTERFACE_VERSION,
  "trace_example",
  "Georg Richter",
  "Trace example plugin",
  {1,0,0},
  "LGPL",
  &trace_init,
  &trace_deinit
};

static char *commands[]= {
  "MYSQL_COM_SLEEP",
  "MYSQL_COM_QUIT",
  "MYSQL_COM_INIT_DB",
  "MYSQL_COM_QUERY",
  "MYSQL_COM_FIELD_LIST",
  "MYSQL_COM_CREATE_DB",
  "MYSQL_COM_DROP_DB",
  "MYSQL_COM_REFRESH",
  "MYSQL_COM_SHUTDOWN",
  "MYSQL_COM_STATISTICS",
  "MYSQL_COM_PROCESS_INFO",
  "MYSQL_COM_CONNECT",
  "MYSQL_COM_PROCESS_KILL",
  "MYSQL_COM_DEBUG",
  "MYSQL_COM_PING",
  "MYSQL_COM_TIME",
  "MYSQL_COM_DELAYED_INSERT",
  "MYSQL_COM_CHANGE_USER",
  "MYSQL_COM_BINLOG_DUMP",
  "MYSQL_COM_TABLE_DUMP",
  "MYSQL_COM_CONNECT_OUT",
  "MYSQL_COM_REGISTER_SLAVE",
  "MYSQL_COM_STMT_PREPARE",
  "MYSQL_COM_STMT_EXECUTE",
  "MYSQL_COM_STMT_SEND_LONG_DATA",
  "MYSQL_COM_STMT_CLOSE",
  "MYSQL_COM_STMT_RESET",
  "MYSQL_COM_SET_OPTION",
  "MYSQL_COM_STMT_FETCH",
  "MYSQL_COM_DAEMON",
  "MYSQL_COM_END"
};

typedef struct {
  unsigned long thread_id;
  int last_command; /* MYSQL_COM_* values, -1 for handshake */
  unsigned int max_packet_size;
  size_t total_size[2];
  unsigned int client_flags;
  char *username;
  char *db;
  char *command;
  char *filename;
  unsigned long refid; /* stmt_id, thread_id for kill */
  uchar charset;
  void *next;
  int local_infile;
} TRACE_INFO;

#define TRACE_STATUS(a) (!a) ? "ok" : "error"

TRACE_INFO *trace_info= NULL;

static TRACE_INFO *get_trace_info(unsigned long thread_id)
{
  TRACE_INFO *info= trace_info;

  while (info)
  {
    if (info->thread_id == thread_id)
      return info;
    else
      info= info->next;
  }

  if (!(info= (TRACE_INFO *)calloc(sizeof(TRACE_INFO), 1)))
    return NULL;
  info->thread_id= thread_id;
  info->next= trace_info;
  trace_info= info;
  return info;
}

static void delete_trace_info(unsigned long thread_id)
{
  TRACE_INFO *last= NULL, *current;
  current= trace_info;

  while (current)
  {
    if (current->thread_id == thread_id)
    {
      printf("deleting thread %d\n", thread_id);

      if (last)
        last->next= current->next;
      else
        trace_info= current->next;
      if (current->command)
        free(current->command);
      if (current->db)
        free(current->db);
      if (current->username)
        free(current->username);
      if (current->filename)
        free(current->filename);
      free(current);
    }
    last= current;
    current= current->next;
  }

}


/* {{{ static int trace_init */
/* 
  Initialization routine

  SYNOPSIS
    trace_init
      unused1
      unused2
      unused3
      unused4

  DESCRIPTION
    Init function registers a callback handler for CIO interface.

  RETURN
    0           success
*/
static int trace_init(char *errormsg, 
                      size_t errormsg_size,
                      int unused1 __attribute__((unused)), 
                      va_list unused2 __attribute__((unused)))
{
  void *func;

#ifdef WIN32
  if (!(func= GetProcAddress(GetModuleHandle(NULL), "ma_cio_register_callback")))
#else
  if (!(func= dlsym(RTLD_DEFAULT, "ma_cio_register_callback")))
#endif
  {
    strncpy(errormsg, "Can't find ma_cio_register_callback function", errormsg_size);
    return 1;
  }
  register_callback= func;
  register_callback(TRUE, trace_callback);

  return 0;
}
/* }}} */

static int trace_deinit()
{
  /* unregister plugin */
  while(trace_info)
  {
    printf("Warning: Connection for thread %d not properly closed\n", trace_info->thread_id);
    trace_info= trace_info->next;
  }
  register_callback(FALSE, trace_callback);
}

static void trace_set_command(TRACE_INFO *info, char *buffer, size_t size)
{
  if (info->command)
    free(info->command);

  info->command= (char *)malloc(size);
  strncpy(info->command, buffer, size);
}

void dump_buffer(uchar *buffer, size_t len)
{
  uchar *p= buffer;
  while (p < buffer + len)
  {
    printf("%02x ", *p);
    p++;
  }
  printf("\n");
}

static void dump_simple(TRACE_INFO *info, my_bool is_error)
{
  printf("%8d: %s %s\n", info->thread_id, commands[info->last_command], TRACE_STATUS(is_error));
}

static void dump_reference(TRACE_INFO *info, my_bool is_error)
{
  printf("%8d: %s(%d) %s\n", info->thread_id, commands[info->last_command], info->refid, TRACE_STATUS(is_error));
}

static void dump_command(TRACE_INFO *info, my_bool is_error)
{
  int i;
  printf("%8d: %s(",  info->thread_id, commands[info->last_command]);
  for (i= 0; info->command && i < strlen(info->command); i++)
    if (info->command[i] == '\n')
      printf("\\n");
    else if (info->command[i] == '\r')
      printf("\\r");
    else if (info->command[i] == '\t')
      printf("\\t");
    else
      printf("%c", info->command[i]);
  printf(") %s\n", TRACE_STATUS(is_error));
}

void trace_callback(int mode, MYSQL *mysql, const uchar *buffer, size_t length)
{
  unsigned long thread_id= mysql->thread_id;
  TRACE_INFO *info;


  /* check if package is server greeting package,
   * and set thread_id */
  if (!thread_id && mode == READ)
  {
    char *p= (char *)buffer;
    p+= 4; /* packet length */
    if (*p != 0xFF) /* protocol version 0xFF indicates error */
    {
      p+= strlen(p + 1) + 2;
      thread_id= uint4korr(p);
    }
    info= get_trace_info(thread_id);
    info->last_command= -1;
  }
  else
  {
    char *p= (char *)buffer;
    info= get_trace_info(thread_id);

    if (info->last_command == -1)
    {
      if (mode == WRITE)
      {
        /* client authentication reply packet:
         * 
         *  ofs description        length
         *  ------------------------
         *  0   length             3
         *  3   packet_no          1
         *  4   client capab.      4
         *  8   max_packet_size    4
         *  12  character set      1
         *  13  reserved          23
         *  ------------------------
         *  36  username (zero terminated)
         *      len (1 byte) + password or
         */

        p+= 4;
        info->client_flags= uint4korr(p);
        p+= 4;
        info->max_packet_size= uint4korr(p);
        p+= 4;
        info->charset= *p;
        p+= 24;
        info->username= strdup(p);
        p+= strlen(p) + 1;
        if (*p) /* we are not interested in authentication data */
          p+= *p;
        p++;
        if (info->client_flags & CLIENT_CONNECT_WITH_DB)
          info->db= strdup(p);
      }
      else
      {
        p++;
        if (*p == 0xFF)
          printf("%8d: CONNECT_ERROR(%d)\n", info->thread_id, uint4korr(p+1));
        else
          printf("%8d: CONNECT_SUCCESS(host=%s,user=%s,db=%s)\n", info->thread_id, 
                 mysql->host, info->username, info->db ? info->db : "'none'");
        info->last_command= MYSQL_COM_SLEEP;
      }
    }
    else {
      char *p= (char *)buffer;
      int len;

      if (mode == WRITE)
      {
        len= uint3korr(p);
        p+= 4;
        info->last_command= *p;
        p++;
        switch (info->last_command) {
        case MYSQL_COM_INIT_DB:
        case MYSQL_COM_DROP_DB:
        case MYSQL_COM_CREATE_DB:
        case MYSQL_COM_DEBUG:
        case MYSQL_COM_QUERY:
        case MYSQL_COM_STMT_PREPARE:
          trace_set_command(info, p, len - 1);
          break;
        case MYSQL_COM_PROCESS_KILL:
          info->refid= uint4korr(p);
          break;
        case MYSQL_COM_QUIT:
          printf("%8d: MYSQL_COM_QUIT\n", info->thread_id);
          delete_trace_info(info->thread_id);
          break;
        case MYSQL_COM_PING:
          printf("%8d: MYSQL_COM_PING\n", info->thread_id);
          break;
        case MYSQL_COM_STMT_EXECUTE:
        case MYSQL_COM_STMT_RESET:
        case MYSQL_COM_STMT_CLOSE:
          info->refid= uint4korr(p);
          break;
        case MYSQL_COM_CHANGE_USER:
          break;
        default:
          if (info->local_infile == 1)
          {
            printf("%8d: SEND_LOCAL_INFILE(%s) ", info->thread_id, info->filename);
            if (len)
              printf("sent %d bytes\n", len);
            else
              printf("- error\n");
            info->local_infile= 2;
          }
          else
            printf("%8d: UNKNOWN_COMMAND: %d\n", info->thread_id, info->last_command);
          break;
        }
      }
      else
      {
        my_bool is_error;

        len= uint3korr(p);
        p+= 4;

        is_error= ((unsigned int)len == -1);

        switch(info->last_command) {
        case MYSQL_COM_STMT_EXECUTE:
        case MYSQL_COM_STMT_RESET:
        case MYSQL_COM_STMT_CLOSE:
        case MYSQL_COM_PROCESS_KILL:
          dump_reference(info, is_error);
          info->refid= 0;
          info->last_command= 0;
          break;
        case MYSQL_COM_QUIT:
          dump_simple(info, is_error);
          break;
        case MYSQL_COM_QUERY:
        case MYSQL_COM_INIT_DB:
        case MYSQL_COM_DROP_DB:
        case MYSQL_COM_CREATE_DB:
        case MYSQL_COM_DEBUG:
        case MYSQL_COM_CHANGE_USER:
          if (info->last_command == MYSQL_COM_QUERY && (uchar)*p == 251)
          {
            info->local_infile= 1;
            p++;
            info->filename= (char *)malloc(len);
            strncpy(info->filename, (char *)p, len);
            dump_command(info, is_error);
            break;
          }
          dump_command(info, is_error);
          if (info->local_infile != 1)
          {
            free(info->command);
            info->command= NULL;
          }
          break;
        case MYSQL_COM_STMT_PREPARE:
          printf("%8d: MYSQL_COM_STMT_PREPARE(%s) ", info->thread_id, info->command);
          if (!*p)
          {
            unsigned long stmt_id= uint4korr(p+1);
            printf("-> stmt_id(%d)\n", stmt_id);
          }
          else
            printf("error\n");
          break;
        }
      }
    }
  }
  info->total_size[mode]+= length;
}
