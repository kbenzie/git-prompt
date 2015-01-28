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

enum gp_error_t {
  GP_SUCCESS = 0,
  GP_ERROR_GET_CURRENT_DIR_FAILED = -1,
  GP_ERROR_OPEN_REPO_FAILED = -2,
  GP_ERROR_DISCOVER_REPO_FAILED = -3,
  GP_ERROR_STATUS_FAILED = -4,
  GP_ERROR_SUBMODULE_ITERATION_FAILED = -5,
  GP_ERROR_GET_REPO_HEAD_FAILED = -6,
  GP_ERROR_AHEAD_BEHIND_FAILED = -7,
  GP_ERROR_REMOTE_LIST_FAILED = -8,
} gp_error;

typedef enum gp_options_t {
  GP_OPTION_NONE = 0,
  GP_OPTION_ENABLE_SUBMODULE_STATUS = 1u << 1,
} gp_options;

typedef struct gp_counters_t {
  size_t staged;
  size_t changed;
  size_t untracked;
  size_t conflicts;
  size_t ahead;
  size_t behind;
} gp_counters;

typedef struct gp_tokens_t {
  char prefix[16];
  char suffix[16];
  char separator[16];
  char staged[16];
  char conflicts[16];
  char changed[16];
  char clean[16];
  char untracked[16];
  char ahead[16];
  char behind[16];
} gp_tokens;

int submoduleCallback(git_submodule *submodule, const char *name, void *payload);

int main(int argc, char **argv) {
  // TODO: Handle arguments
  // * Prompt format
  //   Each prompt element must be supplied other wise it an error.
  //   Should the order be enforced? Or should they be specified by a dash
  //   followed by a mapped letter?
  // * Enable submodule option
  // * ...
  gp_tokens tokens;
  strcpy(tokens.prefix, " ");
  strcpy(tokens.suffix, "");
  strcpy(tokens.separator, " ");
  strcpy(tokens.staged, "*");
  strcpy(tokens.conflicts, "×");
  strcpy(tokens.changed, "+");
  strcpy(tokens.clean, "✓");
  strcpy(tokens.untracked, "…");
  strcpy(tokens.ahead, "↑");
  strcpy(tokens.behind, "↓");

  // TODO: Enforce minimum number of arguments


  gp_options options = 0;

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
    // TODO: How to signify a bare repository?
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

    // NOTE: Determine conflict combination, looking at the status in the
    // debugger seems to imply that if the index and the working tree bits are
    // set then this means the file is in a conflicted state. Is this correct?
    if (entry->status & statusIndexMask && entry->status & statusWtMask) {
      counters.conflicts++;
      continue;
    }

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
  gpCheck(git_repository_head(&head, repo),
          git_repository_free(repo);
          git_libgit2_shutdown(); return GP_ERROR_GET_REPO_HEAD_FAILED);
  const char *branch = git_reference_shorthand(head);

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
  git_repository_free(repo);
  git_libgit2_shutdown();

  // TODO: Construct the prompt string.

  // TODO: Remove these!!!
  printf("branch   : %s\n", branch);
  printf("staged   : %zu\n", counters.staged);
  printf("changed  : %zu\n", counters.changed);
  printf("untracked: %zu\n", counters.untracked);
  printf("conflicts: %zu\n", counters.conflicts);
  printf("ahead    : %zu\n", counters.ahead);
  printf("behind   : %zu\n", counters.behind);
  printf("finished\n");

  if (!counters.staged && !counters.changed && !counters.untracked &&
      !counters.conflicts && !counters.ahead && !counters.behind) {
    printf("%s%s%s", branch, tokens.separator, tokens.clean);
    return 0;
  }

  char prompt[PATH_LENGTH] = "";
  char scratch[PATH_LENGTH] = "";

  strcpy(prompt, branch);

  if (counters.ahead) {
    snprintf(scratch, PATH_LENGTH, "%s%zu", tokens.ahead, counters.ahead);
  }
  if (counters.behind) {
    snprintf(scratch, PATH_LENGTH, "%s%zu", tokens.behind, counters.behind);
  }
  strncat(prompt, scratch, PATH_LENGTH);

  strncat(prompt, tokens.separator, PATH_LENGTH);

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
    snprintf(scratch, PATH_LENGTH, "%s%zu", tokens.conflicts, counters.conflicts);
    strncat(prompt, scratch, PATH_LENGTH);
  }

  printf("%s", prompt);

  return GP_SUCCESS;
}

int submoduleCallback(git_submodule *submodule, const char *name, void *payload) {
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
  const uint32_t statusIndexMask = 0x70;
  const uint32_t statusWtMask = 0x3f80;

  if (status & statusIndexMask) {
    counters->staged++;
  }

  if (status & statusWtMask) {
    counters->untracked++;
  }

  return GP_SUCCESS;
}
