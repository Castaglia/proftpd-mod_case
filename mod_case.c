/*
 * ProFTPD: mod_case -- provides case-insensivity
 * Copyright (c) 2004-2021 TJ Saunders
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
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * This is mod_case, contrib software for proftpd 1.3.x and above.
 * For more information contact TJ Saunders <tj@castaglia.org>.
 */

#include "conf.h"
#include "privs.h"

#define MOD_CASE_VERSION	"mod_case/0.9"

/* Make sure the version of proftpd is as necessary. */
#if PROFTPD_VERSION_NUMBER < 0x0001030402
# error "ProFTPD 1.3.4rc2 or later required"
#endif

static int case_engine = FALSE;
static int case_logfd = -1;

static const char *trace_channel = "case";

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

static void case_replace_copy_paths(cmd_rec *cmd, const char *proto,
    const char *src_path, const char *dst_path) {

  /* Minor nit: if src_path/dst_path is "//", then reduce it to just "/". */
  if (strcmp(src_path, "//") == 0) {
    src_path = pstrdup(cmd->tmp_pool, "/");
  }

  if (strcmp(dst_path, "//") == 0) {
    dst_path = pstrdup(cmd->tmp_pool, "/");
  }

  if (strcmp(proto, "ftp") == 0 ||
      strcmp(proto, "ftps") == 0) {
    array_header *argv;

    /* We should only be handling SITE COPY (over FTP/FTPS) requests here */

    argv = make_array(cmd->pool, 4, sizeof(char *));
    *((char **) push_array(argv)) = pstrdup(cmd->pool, cmd->argv[0]);
    *((char **) push_array(argv)) = pstrdup(cmd->pool, cmd->argv[1]);
    *((char **) push_array(argv)) = pstrdup(cmd->pool, src_path);
    *((char **) push_array(argv)) = pstrdup(cmd->pool, dst_path);

    cmd->argc = argv->nelts;

    *((char **) push_array(argv)) = NULL;
    cmd->argv = argv->elts;

    cmd->arg = pstrcat(cmd->pool, cmd->argv[1], " ", src_path, " ", dst_path,
      NULL);
  }

  pr_cmd_clear_cache(cmd);
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

  if (strcmp(proto, "sftp") == 0) {
    /* We should only be handling SFTP SYMLINK and LINK requests here. */

    cmd->arg = pstrcat(cmd->pool, src_path, "\t", dst_path, NULL);

    if (cmd->argv[1] != cmd->arg) {
      cmd->argv[1] = cmd->arg;
    }
  }

  pr_cmd_clear_cache(cmd);
}

static void case_replace_path(cmd_rec *cmd, const char *proto, const char *path,
    int path_index) {

  if (strcmp(proto, "ftp") == 0 ||
      strcmp(proto, "ftps") == 0) {

    /* Special handling of LIST/NLST/STAT commands, which can take options */
    if (pr_cmd_cmp(cmd, PR_CMD_LIST_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_NLST_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_STAT_ID) == 0) {

      /* XXX Be sure to overwrite the entire cmd->argv array, not just
       * cmd->arg.
       */

      if (path_index > 0) {
        unsigned int i;
        char *arg;

        arg = pstrdup(cmd->tmp_pool, cmd->arg);
        arg[path_index] = '\0';
        arg = pstrcat(cmd->pool, arg, path, NULL);
        cmd->arg = arg;

        /* We also need to find the index into cmd->argv to replace.  Look
         * for the first item that does not start with `-`.
         */
        for (i = 1; i < cmd->argc; i++) {
          if (*((char *) cmd->argv[i]) != '-') {
            break;
          }
        }

        cmd->argv[i] = pstrdup(cmd->pool, path);

      } else {
        cmd->arg = pstrdup(cmd->pool, path);
      }

      pr_cmd_clear_cache(cmd);

    } else {
      char *arg, *dup_path;
      array_header *argv;
      int flags = PR_STR_FL_PRESERVE_COMMENTS;

      dup_path = pstrdup(cmd->pool, path);

      /* Be sure to overwrite the entire cmd->argv array, not just cmd->arg. */
      argv = make_array(cmd->pool, 2, sizeof(char *));
      *((char **) push_array(argv)) = pstrdup(cmd->pool, cmd->argv[0]);

      if (pr_cmd_cmp(cmd, PR_CMD_SITE_ID) == 0) {
        if (strncmp(cmd->argv[1], "CHGRP", 6) == 0 ||
            strncmp(cmd->argv[1], "CHMOD", 6) == 0) {

          *((char **) push_array(argv)) = pstrdup(cmd->pool, cmd->argv[1]);
          *((char **) push_array(argv)) = pstrdup(cmd->pool, cmd->argv[2]);

        } else if (strncmp(cmd->argv[1], "CPFR", 5) == 0 ||
                   strncmp(cmd->argv[1], "CPTO", 5) == 0) {
          *((char **) push_array(argv)) = pstrdup(cmd->pool, cmd->argv[1]);
        }
      }

      /* Handle spaces in the new path properly by breaking them up and adding
       * them into the argv.
       */
      arg = pr_str_get_word(&dup_path, flags);
      while (arg != NULL) {
        pr_signals_handle();

        *((char **) push_array(argv)) = pstrdup(cmd->pool, arg);
        arg = pr_str_get_word(&dup_path, flags);
      }

      cmd->argc = argv->nelts;

      *((char **) push_array(argv)) = NULL;
      cmd->argv = argv->elts;

      pr_cmd_clear_cache(cmd);

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
        cmd->arg = pstrdup(cmd->pool, path);
      }
    }

    if (pr_trace_get_level(trace_channel) >= 19) {
      register unsigned int i;

      pr_trace_msg(trace_channel, 19, "replacing path: cmd->argc = %d",
        cmd->argc);
      for (i = 0; i < cmd->argc; i++) {
        pr_trace_msg(trace_channel, 19, "replacing path: cmd->argv[%u] = '%s'",
          i, (char *) cmd->argv[i]);
      }
    }

    return;
  }

  if (strcmp(proto, "sftp") == 0) {
    /* Main SFTP commands */
    if (pr_cmd_cmp(cmd, PR_CMD_RETR_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_STOR_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_MKD_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_RMD_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_RNFR_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_RNTO_ID) == 0 ||
        pr_cmd_cmp(cmd, PR_CMD_DELE_ID) == 0 ||
        pr_cmd_strcmp(cmd, "LSTAT") == 0 ||
        pr_cmd_strcmp(cmd, "OPENDIR") == 0 ||
        pr_cmd_strcmp(cmd, "READLINK") == 0 ||
        pr_cmd_strcmp(cmd, "REALPATH") == 0 ||
        pr_cmd_strcmp(cmd, "SETSTAT") == 0 ||
        pr_cmd_strcmp(cmd, "STAT") == 0) {
      cmd->arg = pstrdup(cmd->pool, path);
    }
    pr_cmd_clear_cache(cmd);

    return;
  }
}

static int case_scan_directory(pool *p, DIR *dirh, const char *dir_name,
    const char *file, char **matched_file) {
  struct dirent *dent;
  const char *file_match;

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
  while (dent != NULL) {
    pr_signals_handle();

    if (strcmp(dent->d_name, file) == 0) {
      pr_trace_msg(trace_channel, 9,
       "found exact match for file '%s' in directory '%s'", file, dir_name);
      *matched_file = NULL;
      return 0;
    }

    if (pr_fnmatch(file_match, dent->d_name, PR_FNM_CASEFOLD) == 0) {
      (void) pr_log_writefile(case_logfd, MOD_CASE_VERSION,
        "found case-insensitive match '%s' for '%s' in directory '%s'",
        dent->d_name, file_match, dir_name);
      *matched_file = pstrdup(p, dent->d_name);
      return 0;
    }

    dent = pr_fsio_readdir(dirh);
  }

  errno = ENOENT;
  return -1;
}

static const char *case_normalize_path(pool *p, const char *path,
    int *changed) {
  register unsigned int i;
  int xerrno;
  char *iter_path, *normalized_path, **elts;
  size_t path_len;
  pr_fh_t *fh;
  array_header *components;
  pool *tmp_pool;

  /* Special cases. */
  path_len = strlen(path);
  if (path_len == 1) {
    if (path[0] == '/' ||
        path[1] == '.') {
      /* Nothing to do. */
      return path;
    }
  }

  /* Can we open the path as is?  If so, we can avoid the more expensive
   * filesystem walk.  Note that the path might point to a directory.
   */
  fh = pr_fsio_open(path, O_RDONLY);
  xerrno = errno;

  if (fh != NULL) {
    (void) pr_fsio_close(fh);
    return path;
  }

  if (xerrno != ENOENT) {
    /* The path exists as is; that's OK. */
    return path;
  }

  tmp_pool = make_sub_pool(p);

  /* Note that it is tempting to use `pr_fs_split_path()`, however its
   * semantics (resolving to an absolute path first) are not quite expected
   * here.  So we'll just use pr_str_text_to_array() directly.
   */
  components = pr_str_text_to_array(tmp_pool, path, '/');

  /* For the first component, what is the directory to open?  Depends;
   * did the path start with '/', '.', or neither?
   */
  iter_path = pstrdup(tmp_pool, ".");
  if (*path == '/') {
    iter_path = pstrdup(tmp_pool, "/");
  }

  elts = components->elts;
  for (i = 0; i < components->nelts; i++) {
    int res;
    pool *iter_pool;
    DIR *dirh;
    char *matched_elt = NULL;

    /* Note that the last component in the list should be the target; we
     * don't want to use opendir(3) on the target.
     */
    iter_pool = make_sub_pool(tmp_pool);

    dirh = pr_fsio_opendir(iter_path);
    if (dirh == NULL) {
      int xerrno = errno;

      /* This should never happen, right? It could, due to races with other
       * processes' changes to the filesystem.
       */
      (void) pr_log_writefile(case_logfd, MOD_CASE_VERSION,
        "error opening directory '%s': %s", iter_path, strerror(xerrno));
      destroy_pool(iter_pool);

      errno = xerrno;
      return NULL;
    }

    res = case_scan_directory(iter_pool, dirh, iter_path, elts[i],
      &matched_elt);
    if (res == 0 &&
        matched_elt != NULL) {
      ((char **) components->elts)[i] = pstrdup(tmp_pool, matched_elt);

      if (changed != NULL) {
        *changed = TRUE;
      }
    }

    pr_fsio_closedir(dirh);
    destroy_pool(iter_pool);

    iter_path = pdircat(tmp_pool, iter_path, elts[i], NULL);
  }

  /* Now return the normalized path, built from our possibly-modified
   * components.  We would use `pr_fs_join_join()`, but it has a now-corrected
   * bug.
   */
  elts = components->elts;
  if (*path == '/') {
    normalized_path = pstrcat(p, "/", elts[0], NULL);

  } else {
    normalized_path = pstrdup(p, elts[0]);
  }

  for (i = 1; i < components->nelts; i++) {
    char *elt;

    elt = ((char **) components->elts)[i];
    normalized_path = pdircat(p, normalized_path, elt, NULL);
  }

  destroy_pool(tmp_pool);

  pr_trace_msg(trace_channel, 19, "normalized path '%s' to '%s'", path,
    normalized_path);
  return normalized_path;
}

static int case_have_file(pool *p, const char *path,
    const char **matched_path) {
  int changed = FALSE;
  const char *normalized_path;

  normalized_path = case_normalize_path(p, path, &changed);
  if (normalized_path == NULL) {
    return FALSE;
  }

  if (changed == TRUE) {
    *matched_path = normalized_path;
  }

  return TRUE;
}

/* Command handlers
 */

/* The SITE COPY requests are different enough to warrant their own command
 * handler.
 */
MODRET case_pre_copy(cmd_rec *cmd) {
  config_rec *c;
  const char *proto, *matched_path = NULL;
  char *src_path, *dst_path;
  int modified_arg = FALSE, res;

  if (case_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  c = find_config(CURRENT_CONF, CONF_PARAM, "CaseIgnore", FALSE);
  if (c == NULL) {
    return PR_DECLINED(cmd);
  }

  if (*((unsigned int *) c->argv[0]) != TRUE) {
    return PR_DECLINED(cmd);
  }

  if (c->argv[1] != NULL &&
      case_expr_eval_cmds(cmd, *((array_header **) c->argv[1])) == 0) {
    return PR_DECLINED(cmd);
  }

  proto = pr_session_get_protocol(0);

  if (strncasecmp(cmd->argv[2], "HELP", 5) == 0) {
    /* Ignore SITE COPY HELP requests */
    return PR_DECLINED(cmd);
  }

  /* We know the protocol here will always be "ftp" or "ftps", right? And that
   * we are only handling SITE COPY requests here.
   */

  if (cmd->argc != 4) {
    /* Malformed SITE COPY cmd_rec */
    (void) pr_log_writefile(case_logfd, MOD_CASE_VERSION,
      "malformed SITE COPY request, ignoring");
    return PR_DECLINED(cmd);
  }

  src_path = cmd->argv[2];
  dst_path = cmd->argv[3];

  pr_trace_msg(trace_channel, 9,
    "checking client-sent source path '%s', destination path '%s'", src_path,
    dst_path);

  res = case_have_file(cmd->tmp_pool, src_path, &matched_path);
  if (res < 0) {
    return PR_DECLINED(cmd);
  }

  if (res == TRUE &&
      matched_path != NULL) {
    /* Replace the source path */
    src_path = pstrdup(cmd->tmp_pool, matched_path);
    modified_arg = TRUE;

  } else {
    pr_trace_msg(trace_channel, 9,
      "no case-insensitive matches found for path '%s'", src_path);
  }

  matched_path = NULL;
  res = case_have_file(cmd->tmp_pool, dst_path, &matched_path);
  if (res == TRUE) {
    if (matched_path != NULL) {
      /* Replace the destination path */
      dst_path = pstrdup(cmd->tmp_pool, matched_path);
      modified_arg = TRUE;
    }

  } else {
    pr_trace_msg(trace_channel, 9,
      "no case-insensitive matches found for path '%s'", dst_path);
  }

  /* Overwrite the client-given paths. */
  if (modified_arg == TRUE) {
    case_replace_copy_paths(cmd, proto, src_path, dst_path);
  }

  return PR_DECLINED(cmd);
}

MODRET case_pre_cmd(cmd_rec *cmd) {
  config_rec *c;
  const char *proto = NULL, *matched_path = NULL;
  char *path = NULL;
  int path_index = -1, res;

  if (case_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  c = find_config(CURRENT_CONF, CONF_PARAM, "CaseIgnore", FALSE);
  if (c == NULL) {
    return PR_DECLINED(cmd);
  }

  if (*((unsigned int *) c->argv[0]) != TRUE) {
    return PR_DECLINED(cmd);
  }

  if (c->argv[1] != NULL &&
      case_expr_eval_cmds(cmd, *((array_header **) c->argv[1])) == 0) {
    return PR_DECLINED(cmd);
  }

  proto = pr_session_get_protocol(0);

  if (strcmp(proto, "sftp") == 0) {
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

      /* Make sure we operate on a duplicate of the extracted path. */
      path = pstrdup(cmd->tmp_pool, path);

    } else if (pr_cmd_cmp(cmd, PR_CMD_SITE_ID) == 0) {
      register unsigned int i;

      if (strncmp(cmd->argv[1], "COPY", 5) == 0) {
        return case_pre_copy(cmd);
      }

      if (strncmp(cmd->argv[1], "CHGRP", 6) == 0 ||
          strncmp(cmd->argv[1], "CHMOD", 6) == 0) {

        if (cmd->argc < 4) {
          pr_trace_msg(trace_channel, 3,
            "ignoring SITE %s: not enough parameters (%d)",
            (char *) cmd->argv[1], cmd->argc - 2);
          return PR_DECLINED(cmd);
        }

        path = "";

        /* Skip over "SITE, "CHMOD" (or "CHGRP"), and the mode (or group). */
        for (i = 3; i < cmd->argc; i++) {
          path = pstrcat(cmd->tmp_pool, path, *path ? " " : "",
            pr_fs_decode_path(cmd->tmp_pool, cmd->argv[i]), NULL);
        }

      } else if (strncmp(cmd->argv[1], "CPFR", 5) == 0 ||
                 strncmp(cmd->argv[1], "CPTO", 5) == 0) {

        if (cmd->argc < 3) {
          pr_trace_msg(trace_channel, 3,
            "ignoring SITE %s: not enough parameters (%d)",
            (char *) cmd->argv[1], cmd->argc - 2);
          return PR_DECLINED(cmd);
        }

        path = "";

        /* Skip over "SITE, and "CPFR" (or "CPTO"). */
        for (i = 2; i < cmd->argc; i++) {
          path = pstrcat(cmd->tmp_pool, path, *path ? " " : "",
            pr_fs_decode_path(cmd->tmp_pool, cmd->argv[i]), NULL);
        }

      } else {
        (void) pr_log_writefile(case_logfd, MOD_CASE_VERSION,
          "unsupported SITE %s command, ignoring", (char *) cmd->argv[1]);
        return PR_DECLINED(cmd);
      }

    } else {
      path = pstrdup(cmd->tmp_pool, cmd->arg);
    }
  }

  pr_trace_msg(trace_channel, 9, "checking client-sent path '%s'", path);
  res = case_have_file(cmd->tmp_pool, path, &matched_path);
  if (res < 0) {
    return PR_DECLINED(cmd);
  }

  if (res == FALSE) {
    /* No match found. */
    pr_trace_msg(trace_channel, 9,
      "no case-insensitive matches found for path '%s'", path);
    return PR_DECLINED(cmd);
  }

  /* We found a match for the given file. */

  if (matched_path == NULL) {
    /* Exact match found; nothing more to do. */
    return PR_DECLINED(cmd);
  }

  /* Overwrite the client-given path. */
  pr_trace_msg(trace_channel, 9, "replacing path '%s' with '%s'",
    path, matched_path);

  case_replace_path(cmd, proto, matched_path, path_index);
  return PR_DECLINED(cmd);
}

/* The SYMLINK/LINK SFTP requests are different enough to warrant their own
 * command handler.
 */
MODRET case_pre_link(cmd_rec *cmd) {
  config_rec *c;
  const char *proto = NULL, *matched_path = NULL;
  char *arg = NULL, *src_path, *dst_path, *ptr;
  int modified_arg = FALSE, res;

  if (case_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  c = find_config(CURRENT_CONF, CONF_PARAM, "CaseIgnore", FALSE);
  if (c == NULL) {
    return PR_DECLINED(cmd);
  }

  if (*((unsigned int *) c->argv[0]) != TRUE) {
    return PR_DECLINED(cmd);
  }

  if (c->argv[1] != NULL &&
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
      "malformed SFTP %s request, ignoring", (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  *ptr = '\0';

  src_path = arg;
  dst_path = ptr + 1;

  pr_trace_msg(trace_channel, 9,
    "checking client-sent source path '%s', destination path '%s'", src_path,
    dst_path);

  res = case_have_file(cmd->tmp_pool, src_path, &matched_path);
  if (res == TRUE) {
    if (matched_path != NULL) {
      /* Replace the source path */
      src_path = pstrdup(cmd->tmp_pool, matched_path);
      modified_arg = TRUE;
    }

  } else {
    pr_trace_msg(trace_channel, 9,
      "no case-insensitive matches found for path '%s'", src_path);
  }

  matched_path = NULL;
  res = case_have_file(cmd->tmp_pool, dst_path, &matched_path);
  if (res == TRUE) {
    if (matched_path != NULL) {
      /* Replace the destination path */
      dst_path = pstrdup(cmd->tmp_pool, matched_path);
      modified_arg = TRUE;
    }

  } else {
    pr_trace_msg(trace_channel, 9,
      "no case-insensitive matches found for path '%s'", dst_path);
  }

  /* Overwrite the client-given paths. */
  if (modified_arg == TRUE) {
    pr_trace_msg(trace_channel, 9, "replacing %s paths with '%s' and '%s'",
      (char *) cmd->argv[0], src_path, dst_path);
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
  unsigned int argc;
  int ignore = FALSE;
  char **argv;
  config_rec *c;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|CONF_DIR);
  CHECK_ARGS(cmd, 1);

  ignore = get_boolean(cmd, 1);

  c = add_config_param(cmd->argv[0], 2, NULL, NULL);
  c->flags |= CF_MERGEDOWN_MULTI;

  c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
  *((unsigned int *) c->argv[0]) = 1;

  if (ignore != -1) {
    *((unsigned int *) c->argv[0]) = ignore;
    return PR_HANDLED(cmd);
  }

  /* Parse the parameter as a command list. */
  argc = cmd->argc-1;
  argv = (char **) cmd->argv;

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

  if (case_engine == FALSE) {
    return 0;
  }

  c = find_config(main_server->conf, CONF_PARAM, "CaseLog", FALSE);
  if (c == NULL) {
    return 0;
  }

  if (strncasecmp((char *) c->argv[0], "none", 5) != 0) {
    int res, xerrno;

    pr_signals_block();
    PRIVS_ROOT
    res = pr_log_openfile((char *) c->argv[0], &case_logfd, 0660);
    xerrno = errno;
    PRIVS_RELINQUISH
    pr_signals_unblock();

    if (res < 0) {
      pr_log_pri(PR_LOG_NOTICE, MOD_CASE_VERSION
        ": error opening CaseLog '%s': %s", (char *) c->argv[0],
        strerror(xerrno));
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
  { PRE_CMD,	C_SITE,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_SIZE,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_STAT,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_STOR,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_XCWD,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_XMKD,	G_NONE, case_pre_cmd,	TRUE,	FALSE },
  { PRE_CMD,	C_XRMD,	G_NONE, case_pre_cmd,	TRUE,	FALSE },

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
