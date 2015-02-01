#include <unistd.h>

#include <stdio.h>
#include <string.h>

#include <git2.h>

#define gpCheck(error, action)               \
  if (error) {                               \
    printf("git-prompt error: %d\n", error); \
    action;                                  \
  }

#define PATH_LENGTH 4096
#define TOKEN_LENGTH 64

enum gp_error_t {
  GP_SUCCESS = 0,
  GP_HELP = 1,
  GP_ERROR_GET_CURRENT_DIR_FAILED = -1,
  GP_ERROR_OPEN_REPO_FAILED = -2,
  GP_ERROR_DISCOVER_REPO_FAILED = -3,
  GP_ERROR_STATUS_FAILED = -4,
  GP_ERROR_SUBMODULE_ITERATION_FAILED = -5,
  GP_ERROR_GET_REPO_HEAD_FAILED = -6,
  GP_ERROR_AHEAD_BEHIND_FAILED = -7,
  GP_ERROR_REMOTE_LIST_FAILED = -8,
  GP_ERROR_INVALID_ARGUMENT = -9,
} gp_error;

typedef enum gp_option_t {
  GP_OPTION_NONE = 0,
  GP_OPTION_ENABLE_SUBMODULE_STATUS = 1u << 1,
  GP_OPTION_ENABLE_DEBUG_OUTPUT = 1u << 2,
} gp_option_t;

typedef int gp_options;

typedef struct gp_counters_t {
  size_t staged;
  size_t changed;
  size_t untracked;
  size_t conflicts;
  size_t ahead;
  size_t behind;
} gp_counters;

typedef struct gp_tokens_t {
  char prefix[TOKEN_LENGTH];
  char suffix[TOKEN_LENGTH];
  char separator[TOKEN_LENGTH];
  char branch[TOKEN_LENGTH];
  char nohead[TOKEN_LENGTH];
  char staged[TOKEN_LENGTH];
  char conflicts[TOKEN_LENGTH];
  char changed[TOKEN_LENGTH];
  char clean[TOKEN_LENGTH];
  char untracked[TOKEN_LENGTH];
  char ahead[TOKEN_LENGTH];
  char behind[TOKEN_LENGTH];
} gp_tokens;

int parseArgs(int argc, char **argv, gp_tokens *tokens, gp_options *options);

int submoduleCallback(git_submodule *submodule, const char *name,
                      void *payload);

void printClean(gp_tokens *tokens, const char *branch);

int main(int argc, char **argv) {
  gp_tokens tokens = {
      "(", ")", "|", "", "∅", "●", "×", "+", "✓", "…", "↓", "↑",
  };
  gp_options options = 0;
  int error = parseArgs(argc, argv, &tokens, &options);
  if (error) {
    return error;
  }

  if (options & GP_OPTION_ENABLE_DEBUG_OUTPUT) {
    printf(
        "prefix    '%s'\n"
        "suffix    '%s'\n"
        "separator '%s'\n"
        "branch    '%sbranch'\n"
        "nohead    ' %s\n'"
        "staged    '%s'\n"
        "conflicts '%s'\n"
        "changed   '%s'\n"
        "clean     '%s'\n"
        "ahead     '%s'\n"
        "behind    '%s'\n",
        tokens.prefix, tokens.suffix, tokens.separator, tokens.branch,
        tokens.nohead, tokens.staged, tokens.conflicts, tokens.changed,
        tokens.clean, tokens.ahead, tokens.behind);
  }

  // NOTE: Get current working directory.
  char currentDir[PATH_LENGTH];
  if (!getcwd(currentDir, sizeof(currentDir))) {
    return GP_ERROR_GET_CURRENT_DIR_FAILED;
  }

  // NOTE: Initialise libgit2, must be shutdown!
  git_libgit2_init();

  // NOTE: Discover the current git repository.
  // TODO: Should ceiling_dirs be set? Configurable?
  git_buf repoDir = {};
  gpCheck(git_repository_discover(&repoDir, currentDir, 1, NULL),
          return GP_ERROR_DISCOVER_REPO_FAILED);

  // NOTE: Open the repository.
  git_repository *repo = NULL;
  gpCheck(git_repository_open(&repo, repoDir.ptr), git_buf_free(&repoDir);
          git_libgit2_shutdown(); return GP_ERROR_OPEN_REPO_FAILED);

  // NOTE: No longer need the path to the repository.
  git_buf_free(&repoDir);

  if (git_repository_is_bare(repo)) {
    // TODO: Should we add a symbol for bare repositories?
    printf("repository is bare!");
  }

  // NOTE: Recurse over the repository status.
  git_status_list *status;
  git_status_options statusOpts = {};
  statusOpts.version = GIT_STATUS_OPTIONS_VERSION;
  statusOpts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  // NOTE: Not recursing into submodules is much faster! Also we don't need to
  // perform pattern matching or care if the repository has been updated during
  // running of this tool as the expected run time is very short.
  statusOpts.flags =
      GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_EXCLUDE_SUBMODULES |
      GIT_STATUS_OPT_DISABLE_PATHSPEC_MATCH | GIT_STATUS_OPT_NO_REFRESH;
  gpCheck(git_status_list_new(&status, repo, &statusOpts),
          git_repository_free(repo);
          git_libgit2_shutdown(); return GP_ERROR_STATUS_FAILED);

  size_t count = git_status_list_entrycount(status);

  gp_counters counters = {};

  // NOTE: We don't care the actual status of the files, only that there has
  // been a change which is being tracked in the index or the working tree.
  // These masks cover the ranges of possible values for each in the
  // git_status_t enum.
  const uint32_t statusIndexMask =
      GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED |
      GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED |
      GIT_STATUS_INDEX_TYPECHANGE;
  const uint32_t statusWtMask =
      GIT_STATUS_WT_NEW | GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_DELETED |
      GIT_STATUS_WT_TYPECHANGE | GIT_STATUS_WT_RENAMED |
      GIT_STATUS_WT_UNREADABLE;

  const git_status_entry *entry;
  for (size_t index = 0; index < count; ++index) {
    entry = git_status_byindex(status, index);

    if (entry->status & GIT_STATUS_CURRENT) {
      continue;
    }

// TODO: Find the correct way to determine the entry is in conflict.
#if GP_EXPERIMENTAL
    if (entry->status & statusIndexMask && entry->status & statusWtMask) {
      counters.conflicts++;
      continue;
    }
#endif

    if (entry->status & statusIndexMask) {
      counters.staged++;
      continue;
    }

    if (entry->status & (statusWtMask ^ GIT_STATUS_WT_NEW)) {
      counters.changed++;
      continue;
    }

    if (entry->status & GIT_STATUS_WT_NEW) {
      counters.untracked++;
      continue;
    }
  }

  // NOTE: Getting the status of submodule is slow, particularly for mane large
  // repositories so it disabled by default.
  if (options & GP_OPTION_ENABLE_SUBMODULE_STATUS) {
    gpCheck(git_submodule_foreach(repo, &submoduleCallback, &counters),
            git_repository_free(repo);
            git_libgit2_shutdown(); return GP_ERROR_SUBMODULE_ITERATION_FAILED);
  }

  // NOTE: Get current branch name.
  git_reference *head = NULL;
  const char *branch = NULL;
  // TODO: Current we error if a reposotory has no HEAD, this should be allowed.
  error = git_repository_head(&head, repo);
  if (error) {
    if (GIT_EUNBORNBRANCH == error) {
      branch = "∅";
    } else {
      git_repository_free(repo);
      git_libgit2_shutdown();
      return GP_ERROR_GET_REPO_HEAD_FAILED;
    }
  } else {
    branch = git_reference_shorthand(head);
  }

  if (head) {
    // NOTE: Get a list the possible remote names to determine how far ahead or
    // behind the local HEAD is.
    git_strarray remoteList;
    gpCheck(git_remote_list(&remoteList, repo), git_reference_free(head);
            git_repository_free(repo); git_libgit2_shutdown();
            return GP_ERROR_REMOTE_LIST_FAILED);

    // NOTE: Get the number of commits ahead/behind remote.
    if (remoteList.count) {
      const git_oid *local = git_reference_target(head);

      // TODO: Is the first entry in the remoteList actually the default as we
      // assume?

      // NOTE: Construct the full remote name.
      char remoteName[PATH_LENGTH];
      strcpy(remoteName, "refs/remotes/");
      strcat(remoteName, remoteList.strings[0]);
      strcat(remoteName, "/");
      strcat(remoteName, branch);

      // NOTE: Use the remote branch name to
      git_oid upstream;
      int error = git_reference_name_to_id(&upstream, repo, remoteName);

      if (local && !error) {
        gpCheck(git_graph_ahead_behind(&counters.ahead, &counters.behind, repo,
                                       local, &upstream),
                git_reference_free(head);
                git_repository_free(repo); git_libgit2_shutdown();
                return GP_ERROR_AHEAD_BEHIND_FAILED);
      }
    }

    // NOTE: Clean up allocated resources.
    git_reference_free(head);
  }
  git_repository_free(repo);
  git_libgit2_shutdown();

  // NOTE: Check for a clean repo
  if (!counters.staged && !counters.changed && !counters.untracked &&
      !counters.conflicts && !counters.ahead && !counters.behind) {
    printClean(&tokens, branch);
    return GP_SUCCESS;
  }

  char prompt[PATH_LENGTH] = "";
  char scratch[PATH_LENGTH] = "";

  snprintf(prompt, PATH_LENGTH, "%s%s", tokens.branch, branch);

  if (counters.ahead) {
    snprintf(scratch, PATH_LENGTH, "%s%zu", tokens.ahead, counters.ahead);
  }
  if (counters.behind) {
    snprintf(scratch, PATH_LENGTH, "%s%zu", tokens.behind, counters.behind);
  }
  strncat(prompt, scratch, PATH_LENGTH);

  if (counters.staged || counters.changed || counters.untracked ||
      counters.conflicts) {
    strncat(prompt, tokens.separator, PATH_LENGTH);
  }

  if (counters.staged) {
    snprintf(scratch, PATH_LENGTH, "%s%zu", tokens.staged, counters.staged);
    strncat(prompt, scratch, PATH_LENGTH);
  }
  if (counters.changed) {
    snprintf(scratch, PATH_LENGTH, "%s%zu", tokens.changed, counters.changed);
    strncat(prompt, scratch, PATH_LENGTH);
  }
  if (counters.untracked) {
    snprintf(scratch, PATH_LENGTH, "%s", tokens.untracked);
    strncat(prompt, scratch, PATH_LENGTH);
  }
  if (counters.conflicts) {
    snprintf(scratch, PATH_LENGTH, "%s%zu", tokens.conflicts,
             counters.conflicts);
    strncat(prompt, scratch, PATH_LENGTH);
  }

  printf("%s%s%s", tokens.prefix, prompt, tokens.suffix);

  return GP_SUCCESS;
}

int parseArgs(int argc, char **argv, gp_tokens *tokens, gp_options *options) {
  for (int argIndex = 1; argIndex < argc; ++argIndex) {
    if (!strcmp("-h", argv[argIndex]) || !strcmp("--help", argv[argIndex])) {
      printf("Usage: %s <options>\n\n", argv[0]);
      printf("Options:\n");
      printf("    -h --help         Show this help dialogue\n");
      printf("    --submodules      Enable submodule status updates\n");
      printf("    --debug           Enable debug output\n");
      printf("    prefix \"%s\"        Change the prefix token to '%s'\n",
             tokens->prefix, tokens->prefix);
      printf("    suffix \"%s\"        Change the suffix token to '%s'\n",
             tokens->suffix, tokens->suffix);
      printf("    branch \"%s\"         Change the branch token to '%s'\n",
             tokens->branch, tokens->branch);
      printf("    nohead \"%s\"        Change the nohead token to '%s'\n",
             tokens->nohead, tokens->nohead);
      printf("    separator \"%s\"     Change the separator token to '%s'\n",
             tokens->separator, tokens->separator);
      printf("    staged \"%s\"        Change the staged token to '%s'\n",
             tokens->staged, tokens->staged);
      printf("    conflicts \"%s\"     Change the conflicts token to '%s'\n",
             tokens->conflicts, tokens->conflicts);
      printf("    changed \"%s\"       Change the changed token to '%s'\n",
             tokens->changed, tokens->changed);
      printf("    clean \"%s\"         Change the clean token to '%s'\n",
             tokens->clean, tokens->clean);
      printf("    untracked \"%s\"     Change the untracked token to '%s'\n",
             tokens->untracked, tokens->untracked);
      printf("    ahead \"%s\"         Change the ahead token to '%s'\n",
             tokens->ahead, tokens->ahead);
      printf("    behind \"%s\"        Change the behind token to '%s'\n",
             tokens->behind, tokens->behind);
      return GP_HELP;
    }
    if (!strcmp("--debug", argv[argIndex])) {
      *options |= GP_OPTION_ENABLE_DEBUG_OUTPUT;
      continue;
    }
    if (!strcmp("--submodules", argv[argIndex])) {
      *options |= GP_OPTION_ENABLE_SUBMODULE_STATUS;
      continue;
    }
    if (!strcmp("prefix", argv[argIndex])) {
      strcpy(tokens->prefix, argv[++argIndex]);
      continue;
    }
    if (!strcmp("suffix", argv[argIndex])) {
      strcpy(tokens->suffix, argv[++argIndex]);
      continue;
    }
    if (!strcmp("branch", argv[argIndex])) {
      strcpy(tokens->branch, argv[++argIndex]);
      continue;
    }
    if (!strcmp("nohead", argv[argIndex])) {
      strcpy(tokens->nohead, argv[++argIndex]);
      continue;
    }
    if (!strcmp("separator", argv[argIndex])) {
      strcpy(tokens->separator, argv[++argIndex]);
      continue;
    }
    if (!strcmp("staged", argv[argIndex])) {
      strcpy(tokens->staged, argv[++argIndex]);
      continue;
    }
    if (!strcmp("conflicts", argv[argIndex])) {
      strcpy(tokens->conflicts, argv[++argIndex]);
      continue;
    }
    if (!strcmp("changed", argv[argIndex])) {
      strcpy(tokens->changed, argv[++argIndex]);
      continue;
    }
    if (!strcmp("untracked", argv[argIndex])) {
      strcpy(tokens->untracked, argv[++argIndex]);
      continue;
    }
    if (!strcmp("clean", argv[argIndex])) {
      strcpy(tokens->clean, argv[++argIndex]);
      continue;
    }
    if (!strcmp("ahead", argv[argIndex])) {
      strcpy(tokens->ahead, argv[++argIndex]);
      continue;
    }
    if (!strcmp("behind", argv[argIndex])) {
      strcpy(tokens->behind, argv[++argIndex]);
      continue;
    }
    return GP_ERROR_INVALID_ARGUMENT;
  }

  return GP_SUCCESS;
}

int submoduleCallback(git_submodule *submodule, const char *name,
                      void *payload) {
  gp_counters *counters = (gp_counters *)payload;

  // TODO: Is there a flag to make the status query faster but still provide the
  // desired information? We want to avoid traversing the submodules index if
  // possible, or at least do it more optimally?
  git_submodule_ignore_t ignore =
      git_submodule_set_ignore(submodule, GIT_SUBMODULE_IGNORE_UNTRACKED);

  // NOTE: Querying the statue of a large submodule is slow, this behaviour
  // should be optional and disabled by default.
  uint32_t status;
  gpCheck(git_submodule_status(&status, submodule), return -1);

  // NOTE: Once again we don't care what the actual status of the submodule is
  // to we just test for the status of the index and working tree.
  const uint32_t statusIndexMask = GIT_SUBMODULE_STATUS_INDEX_ADDED |
                                   GIT_SUBMODULE_STATUS_INDEX_DELETED |
                                   GIT_SUBMODULE_STATUS_INDEX_MODIFIED;
  const uint32_t statusWtMask =
      GIT_SUBMODULE_STATUS_WD_UNINITIALIZED | GIT_SUBMODULE_STATUS_WD_ADDED |
      GIT_SUBMODULE_STATUS_WD_DELETED | GIT_SUBMODULE_STATUS_WD_MODIFIED |
      GIT_SUBMODULE_STATUS_WD_INDEX_MODIFIED |
      GIT_SUBMODULE_STATUS_WD_WD_MODIFIED;

  if (status & statusIndexMask) {
    counters->staged++;
  }

  if (status & statusWtMask) {
    counters->changed++;
  }

  if (status & GIT_SUBMODULE_STATUS_WD_UNTRACKED) {
    counters->untracked++;
  }

  return GP_SUCCESS;
}

void printClean(gp_tokens *tokens, const char *branch) {
  printf("%s%s%s%s%s%s", tokens->prefix, tokens->branch, branch,
         tokens->separator, tokens->clean, tokens->suffix);
}
