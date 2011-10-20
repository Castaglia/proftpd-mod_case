/*
 * ProFTPD: mod_case -- provides case-insensivity
 *
 * Copyright (c) 2004-2011 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * This is mod_case, contrib software for proftpd 1.2 and above.
 * For more information contact TJ Saunders <tj@castaglia.org>.
 *
 * $Id: mod_case.c,v 1.9 2011/05/04 21:50:41 tj Exp tj $
 */

#include "conf.h"
#include "privs.h"

#define MOD_CASE_VERSION	"mod_case/0.7"

/* Make sure the version of proftpd is as necessary. */
#if PROFTPD_VERSION_NUMBER < 0x0001030402
# error "ProFTPD 1.3.4rc2 or later required"
#endif

static int case_engine = FALSE;
static int case_logfd = -1;

/* Support routines
 */

static int case_expr_eval_cmds(cmd_rec *cmd, array_header *list) {
  int cmd_id, found;
  register unsigned int i;

  for (i = 0; i < list->nelts; i++) { 
    char *c = ((char **) list->elts)[i];
    found = 0;

    if (*c == '!') {
      found = !found;
      c++;
    }

    cmd_id = pr_cmd_get_id(c);
    if (cmd_id > 0) {
      if (pr_cmd_cmp(cmd, cmd_id) == 0) {
        found = !found;
      }

    } else {
      /* Fallback to doing a full strcmp(3). */
      if (strcmp(cmd->argv[0], c) == 0) {
        found = !found;
      }
    }

    if (found) {
      return 1;
    }
  }

  return 0;
}

static char *case_get_opts_path(cmd_rec *cmd, int *path_index) {
  char *ptr;
  char *path;
  size_t pathlen;

  if (cmd->arg == NULL) {
    return NULL;
  }

  ptr = path = cmd->arg;

  pathlen = strlen(path);
  if (pathlen == 0) {
    return NULL;
  }

  while (isspace((int) *ptr)) {
    pr_signals_handle();
    ptr++;
  }

  if (*ptr == '-') {
    /* Options are found; skip past the leading whitespace. */
    path = ptr;
  }

  while (path &&
         *path == '-') {
    /* Advance to the next whitespace */
    while (*path != '\0' &&
           !isspace((int) *path)) {
      path++;
    }

    ptr = path;

    while (*ptr &&
           isspace((int) *ptr)) {
      pr_signals_handle();
      ptr++;
    }

    if (*ptr == '-') {
      /* Options are found; skip past the leading whitespace. */
      path = ptr;

    } else if (*(path + 1) == ' ') {
      /* If the next character is a blank space, advance just one
       * character.
       */
      path++;
      break;

    } else {
      path = ptr;
      break;
    }
  }

  pathlen = strlen(path);
  if (pathlen == 0) {
    return NULL;
  }

  *path_index = (ptr - cmd->arg);
  return path;
}

static void case_replace_link_paths(cmd_rec *cmd, const char *proto,
    const char *src_path, const char *dst_path) {

  /* Minor nit: if src_path/dst_path is "//", then reduce it to just "/". */
  if (strcmp(src_path, "//") == 0) {
    src_path = pstrdup(cmd->tmp_pool, "/");
  }

  if (strcmp(dst_path, "//") == 0) {
    dst_path = pstrdup(cmd->tmp_pool, "/");
  }

  if (strncmp(proto, "sftp", 5) == 0) {

    /* We should only be handling SFTP SYMLINk and LINK requests here */

    cmd->arg = pstrcat(cmd->pool, src_path, "\t", dst_path, NULL);

    if (cmd->argv[1] != cmd->arg) {
      cmd->argv[1] = cmd->arg;
    }
  }

  return;
}

static void case_replace_path(cmd_rec *cmd, const char *proto, const char *dir,
    const char *file, int path_index) {

  /* Minor nit: if dir is "//", then reduce it to just "/". */
  if (strcmp(dir, "//") == 0) {
    dir = pstrdup(cmd->tmp_pool, "/");
  }

  if (strncmp(proto, "ftp", 4) == 0) {

    /* Special handling of LIST/NLST/STAT commands, which can take options */
    if (pr_cmd_cmp(cmd, PR_CMD_LIST_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_NLST_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_STAT_ID) == 0) {

       if (path_index > 0) {
         char *arg;

         arg = pstrdup(cmd->tmp_pool, cmd->arg);
         arg[path_index] = '\0';
         arg = pstrcat(cmd->pool, arg, dir, file, NULL);
         cmd->arg = arg;

       } else {
        cmd->arg = pstrcat(cmd->pool, dir, file, NULL);
       }

    } else {
      cmd->argv[1] = pstrcat(cmd->pool, dir, file, NULL);

      /* In the case of many commands, we also need to overwrite cmd->arg. */
      if (pr_cmd_cmp(cmd, PR_CMD_APPE_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_CWD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_DELE_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_MKD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_MDTM_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_MLSD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_MLST_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_RETR_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_RMD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_RNFR_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_RNTO_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_SIZE_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_STOR_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_XCWD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_XMKD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_XRMD_ID) == 0) {
        cmd->arg = pstrcat(cmd->pool, dir, file, NULL);
      }
    }

    return;
  }

  if (strncmp(proto, "sftp", 5) == 0) {

    /* Main SFTP commands */
    if (pr_cmd_cmp(cmd, PR_CMD_RETR_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_STOR_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_MKD_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_RMD_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_DELE_ID) == 0 ||
        pr_cmd_strcmp(cmd, "LSTAT") == 0 ||
        pr_cmd_strcmp(cmd, "OPENDIR") == 0 ||
        pr_cmd_strcmp(cmd, "READLINK") == 0 ||
        pr_cmd_strcmp(cmd, "REALPATH") == 0 ||
        pr_cmd_strcmp(cmd, "SETSTAT") == 0 ||
        pr_cmd_strcmp(cmd, "STAT") == 0) {
      cmd->arg = pstrcat(cmd->pool, dir, file, NULL);
    }

    return;
  }
}

static int case_have_file(pool *p, const char *dir, const char *file,
    size_t file_len, char **matched_file) {
  DIR *dirh;
  struct dirent *dent;
  char *file_match;

  /* Open the directory. */
  dirh = pr_fsio_opendir(dir);
  if (dirh == NULL) {
    (void) pr_log_writefile(case_logfd, MOD_CASE_VERSION,
      "error opening directory '%s': %s", dir, strerror(errno));
    return -1;
  }

  /* Escape any existing fnmatch(3) characters in the file name. */
  file_match = pstrdup(p, file);

  if (strchr(file_match, '?') != NULL) {
    file_match = sreplace(p, file_match, "?", "\\?", NULL);
  }

  if (strchr(file_match, '*') != NULL) {
    file_match = sreplace(p, file_match, "*", "\\*", NULL);
  }

  if (strchr(file_match, '[') != NULL) {
    file_match = sreplace(p, file_match, "[", "\\[", NULL);
  }

  /* For each file in the directory, check it against the given name, both
   * as an exact match and as a possible match.
   */
  dent = pr_fsio_readdir(dirh);
  while (dent) {
    pr_signals_handle();

    if (strncmp(dent->d_name, file, file_len + 1) == 0) {
      (void) pr_log_writefile(case_logfd, MOD_CASE_VERSION,
        "found exact match");
      pr_fsio_closedir(dirh);

      *matched_file = NULL;
      return TRUE;
    }

    if (pr_fnmatch(file_match, dent->d_name, PR_FNM_CASEFOLD) == 0) {
      (void) pr_log_writefile(case_logfd, MOD_CASE_VERSION,
        "found case-insensitive match '%s' for '%s'", dent->d_name, file_match);
      pr_fsio_closedir(dirh);

      *matched_file = pstrdup(p, dent->d_name);
      return TRUE;
    }

    dent = pr_fsio_readdir(dirh);
  }

  /* Close the directory. */
  pr_fsio_closedir(dirh);

  return FALSE;
}

/* Command handlers
 */

MODRET case_pre_cmd(cmd_rec *cmd) {
  config_rec *c;
  char *path = NULL, *dir = NULL, *file = NULL, *file_match = NULL, *tmp;
  const char *proto = NULL;
  size_t file_len;
  int path_index = -1, res;

  if (!case_engine) {
    return PR_DECLINED(cmd);
  }

  c = find_config(CURRENT_CONF, CONF_PARAM, "CaseIgnore", FALSE);
  if (c == NULL) {
    return PR_DECLINED(cmd);
  }

  if (*((unsigned int *) c->argv[0]) != TRUE) {
    return PR_DECLINED(cmd);
  }

  if (c->argv[1] &&
      case_expr_eval_cmds(cmd, *((array_header **) c->argv[1])) == 0) {
    return PR_DECLINED(cmd);
  }

  proto = pr_session_get_protocol(0);

  if (strncmp(proto, "sftp", 5) == 0) {
    path = pstrdup(cmd->tmp_pool, cmd->arg);

  } else {
    /* Special handling of LIST/NLST/STAT, given that they may have options
     * in the command.
     */
    if (pr_cmd_cmp(cmd, PR_CMD_LIST_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_NLST_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_STAT_ID) == 0) {
      path = case_get_opts_path(cmd, &path_index);

      /* LIST, NLST, and STAT can send no path arguments.  If that's the
       * case, we're done.
       */
      if (path == NULL) {
        return PR_DECLINED(cmd);
      }

    } else {
      path = pstrdup(cmd->tmp_pool, cmd->argv[1]);
    }
  }

  /* Separate the path into directory and file components. */
  tmp = strrchr(path, '/');
  if (tmp == NULL) {
    dir = ".";
    file = path;

  } else {
    if (tmp != path) {
      *tmp++ = '\0';
      dir = path;
      file = tmp;

    } else {
      /* Handle the case where the path is "/path". */
      dir = "/";
      file = tmp + 1;
    }
  }

  file_len = strlen(file);

  res = case_have_file(cmd->tmp_pool, dir, file, file_len, &file_match);
  if (res < 0) {
    return PR_DECLINED(cmd);
  }

  if (res == FALSE) {
    /* No match found. */
    return PR_DECLINED(cmd);
  }

  /* We found a match for the given file. */

  if (file_match == NULL) {
    /* Exact match found; nothing more to do. */
    return PR_DECLINED(cmd);
  }

  /* Overwrite the client-given path. */
  case_replace_path(cmd, proto,
    tmp ? pstrcat(cmd->pool, dir, "/", NULL) : "", file_match, path_index);

  return PR_DECLINED(cmd);
}

/* The SYMLINK/LINK SFTP requests are different enough to warrant their
 * own command handler.
 */
MODRET case_pre_link(cmd_rec *cmd) {
  config_rec *c;
  char *arg = NULL, *src_path, *src_dir = NULL, *src_file = NULL,
    *dst_path, *dst_dir = NULL, *dst_file = NULL, *file_match = NULL,
    *src_ptr, *dst_ptr, *ptr;
  const char *proto = NULL;
  size_t file_len;
  int modified_arg = FALSE, res;

  if (!case_engine) {
    return PR_DECLINED(cmd);
  }

  c = find_config(CURRENT_CONF, CONF_PARAM, "CaseIgnore", FALSE);
  if (c == NULL) {
    return PR_DECLINED(cmd);
  }

  if (*((unsigned int *) c->argv[0]) != TRUE) {
    return PR_DECLINED(cmd);
  }

  if (c->argv[1] &&
      case_expr_eval_cmds(cmd, *((array_header **) c->argv[1])) == 0) {
    return PR_DECLINED(cmd);
  }

  proto = pr_session_get_protocol(0);

  /* We know the protocol here will always be "sftp", right? And that we
   * are only handling SFTP SYMLINK and LINK requests here.
   */

  arg = pstrdup(cmd->tmp_pool, cmd->arg);

  ptr = strchr(arg, '\t');
  if (ptr == NULL) {
    /* Malformed SFTP SYMLINK/LINK cmd_rec. */
    (void) pr_log_writefile(case_logfd, MOD_CASE_VERSION,
      "malformed SFTP %s request, ignoring", cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  *ptr = '\0';

  src_path = arg;
  dst_path = ptr + 1;

  /* Separate the path into directory and file components. */
  src_ptr = strrchr(src_path, '/');
  if (src_ptr == NULL) {
    src_dir = ".";
    src_file = src_path;

  } else {
    if (src_ptr != src_path) {
      *src_ptr = '\0';
      src_dir = src_path;
      src_file = src_ptr + 1;

    } else {
      /* Handle the case where the path is "/path". */
      src_dir = "/";
      src_file = src_ptr + 1;
    }
  }

  dst_ptr = strrchr(dst_path, '/');
  if (dst_ptr == NULL) {
    dst_dir = ".";
    dst_file = dst_path;

  } else {
    if (dst_ptr != dst_path) {
      *dst_ptr = '\0';
      dst_dir = dst_path;
      dst_file = dst_ptr + 1;

    } else {
      /* Handle the case where the path is "/path". */
      dst_dir = "/";
      dst_file = dst_ptr + 1;
    }
  }

  file_len = strlen(src_file);

  res = case_have_file(cmd->tmp_pool, src_dir, src_file, file_len, &file_match);
  if (res < 0) {
    return PR_DECLINED(cmd);
  }

  if (res == TRUE &&
      file_match != NULL) {
    /* Replace the source path */
    src_path = pdircat(cmd->tmp_pool, src_dir, file_match, NULL);
    modified_arg = TRUE;

  } else {
    /* No match (or exact match) found; restore the original src_path. */
    if (src_ptr != NULL) {
      *src_ptr = '/';
    }
  }

  file_len = strlen(dst_file);
  file_match = NULL;

  res = case_have_file(cmd->tmp_pool, dst_dir, dst_file, file_len, &file_match);
  if (res < 0) {
    return PR_DECLINED(cmd);
  }

  if (res == TRUE &&
      file_match != NULL) {
    /* Replace the destination path */
    dst_path = pdircat(cmd->tmp_pool, dst_dir, file_match, NULL);
    modified_arg = TRUE;

  } else {
    /* No match (or exact match) found; restore the original src_path. */
    if (dst_ptr != NULL) {
      *dst_ptr = '/';
    }
  }

  /* Overwrite the client-given paths. */
  if (modified_arg) {
    case_replace_link_paths(cmd, proto, src_path, dst_path);
  }

  return PR_DECLINED(cmd);
}

/* Configuration handlers
 */

/* usage: CaseEngine on|off */
MODRET set_caseengine(cmd_rec *cmd) {
  int bool;
  config_rec *c;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1);

  bool = get_boolean(cmd, 1);
  if (bool == -1)
    CONF_ERROR(cmd, "expected Boolean parameter");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
  *((unsigned int *) c->argv[0]) = bool;

  return PR_HANDLED(cmd);
}

/* usage: CaseIgnore on|off|cmd-list */
MODRET set_caseignore(cmd_rec *cmd) {
  int bool, argc;
  char **argv;
  config_rec *c;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|CONF_DIR);
  CHECK_ARGS(cmd, 1);

  bool = get_boolean(cmd, 1);

  c = add_config_param(cmd->argv[0], 2, NULL, NULL);
  c->flags |= CF_MERGEDOWN_MULTI;

  c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
  *((unsigned int *) c->argv[0]) = 1;

  if (bool != -1) {
    *((unsigned int *) c->argv[0]) = bool;
    return PR_HANDLED(cmd);
  }

  /* Parse the parameter as a command list. */
  argc = cmd->argc-1;
  argv = cmd->argv;

  c->argv[1] = pcalloc(c->pool, sizeof(array_header *));
  *((array_header **) c->argv[1]) = pr_expr_create(c->pool, &argc, argv);

  return PR_HANDLED(cmd);
}

/* usage: CaseLog path|"none" */
MODRET set_caselog(cmd_rec *cmd) {
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1);

  if (pr_fs_valid_path(cmd->argv[1]) < 0)
    CONF_ERROR(cmd, "must be an absolute path");

  add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);

  return PR_HANDLED(cmd);
}

/* Initialization functions
 */

static int case_sess_init(void) {
  config_rec *c;

  c = find_config(main_server->conf, CONF_PARAM, "CaseEngine", FALSE);
  if (c != NULL &&
      *((unsigned int *) c->argv[0]) == TRUE) {
    case_engine = TRUE;
  }

  if (!case_engine) {
    return 0;
  }

  c = find_config(main_server->conf, CONF_PARAM, "CaseLog", FALSE);
  if (c == NULL)
    return 0;

  if (strncasecmp((char *) c->argv[0], "none", 5) != 0) {
    int res;

    pr_signals_block();
    PRIVS_ROOT
    res = pr_log_openfile((char *) c->argv[0], &case_logfd, 0660);
    PRIVS_RELINQUISH
    pr_signals_unblock();

    if (res < 0) {
      pr_log_pri(PR_LOG_NOTICE, MOD_CASE_VERSION
        ": error opening CaseLog '%s': %s", (char *) c->argv[0],
        strerror(errno)); 
    }
  }

  return 0;
}

/* Module API tables
 */

static conftable case_conftab[] = {
  { "CaseEngine",	set_caseengine,		NULL },
  { "CaseIgnore",	set_caseignore,		NULL },
  { "CaseLog",		set_caselog,		NULL },
  { NULL }
};

static cmdtable case_cmdtab[] = {
  { PRE_CMD,	C_APPE,	G_NONE,	case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_CWD,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_DELE,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_LIST,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_MDTM,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_MKD,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_MLSD,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_MLST,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_NLST,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_RETR,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_RMD,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_RNFR,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_RNTO,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_SIZE,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_STAT,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_STOR,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_XCWD,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_XMKD,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_XRMD,	G_NONE, case_pre_cmd,	TRUE,	FALSE },

  /* The following is for mod_copy's SITE CPFR and SITE CPTO commands. */
  /* XXX Need to handle SITE CHMOD, SITE CHGRP as well */

  /* The following are SFTP requests */
  { PRE_CMD,	"LINK",		G_NONE, case_pre_link,	TRUE,	FALSE },
  { PRE_CMD,	"LSTAT",	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"OPENDIR",	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"READLINK",	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"REALPATH",	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"SETSTAT",	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"STAT",		G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	"SYMLINK",	G_NONE, case_pre_link,	TRUE,	FALSE },

  { 0, NULL }
};

module case_module = {
  NULL, NULL,

  /* Module API version 2.0 */
  0x20,

  /* Module name */
  "case",

  /* Module configuration handler table */
  case_conftab,

  /* Module command handler table */
  case_cmdtab,

  /* Module authentication handler table */
  NULL,

  /* Module initialization function */
  NULL,

  /* Session initialization function */
  case_sess_init,

  /* Module version */
  MOD_CASE_VERSION
};
