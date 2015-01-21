#include <dirent.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>

#include <git2.h>

#define gpCheck(error, action)               \
  if (error) {                               \
    printf("git-prompt error: %d\n", error); \
    action;                                  \
  }

enum gp_error_t {
  GP_SUCCESS = 0,
  GP_ERROR_GET_CURRENT_DIR_FAILED = -1,
  GP_ERROR_OPEN_REPO_FAILED = -2,
  GP_ERROR_DISCOVER_REPO_FAILE = -3,
  GP_ERROR_STATUS_FAILED = -4,
};

// TODO: Prefix
// TODO: Suffix
// TODO: Separator
// TODO: Branch
// TODO: Staged
// TODO: Conflicts
// TODO: Behind
// TODO: Ahead
// TODO: Untracked
// TODO: Clean

int main(int argc, char *argv[]) {
  // TODO: Handle arguments

  // NOTE: Get current working directory.
  char currentDir[PATH_MAX];
  if (!getcwd(currentDir, sizeof(currentDir))) {
    return GP_ERROR_GET_CURRENT_DIR_FAILED;
  }

  // NOTE: Initialise libgit2, must be shutdown!
  git_libgit2_init();

  // NOTE: Discover the current git repository.
  // TODO: Should ceiling_dirs be set? Configurable?
  git_buf repoDir;
  gpCheck(git_repository_discover(&repoDir, currentDir, 1, NULL),
          return GP_ERROR_DISCOVER_REPO_FAILE);

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

  const git_status_entry *entry;
  for (size_t index = 0; index < count; ++index) {
    entry = git_status_byindex(status, index);

    // TODO: Increment counters
    char *istatus = NULL;
    if (entry->status & GIT_STATUS_INDEX_NEW)
      istatus = "staged new file:    ";
    if (entry->status & GIT_STATUS_INDEX_MODIFIED)
      istatus = "staged modified:    ";
    if (entry->status & GIT_STATUS_INDEX_DELETED)
      istatus = "staged deleted:     ";
    if (entry->status & GIT_STATUS_INDEX_RENAMED)
      istatus = "staged renamed:     ";
    if (entry->status & GIT_STATUS_INDEX_TYPECHANGE)
      istatus = "staged typechange:  ";

    if (entry->status & GIT_STATUS_WT_NEW)
      istatus = "unstaged new file:  ";
    if (entry->status & GIT_STATUS_WT_MODIFIED)
      istatus = "unstaged modified:  ";
    if (entry->status & GIT_STATUS_WT_DELETED)
      istatus = "unstaged deleted:   ";
    if (entry->status & GIT_STATUS_WT_RENAMED)
      istatus = "unstaged renamed:   ";
    if (entry->status & GIT_STATUS_WT_TYPECHANGE)
      istatus = "unstaged typechange:";

    printf("%s: %s\n", istatus, entry->index_to_workdir->new_file.path);
  }

  // TODO: Find a fast way to determine if submodules are dirty!

  // NOTE: Clean up allocated resources.
  git_repository_free(repo);
  git_libgit2_shutdown();

  // TODO: Make this debug only!
  printf("finished\n");

  return 0;
}
