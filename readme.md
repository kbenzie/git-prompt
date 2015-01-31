# git-prompt a native git prompt status tool

This project aims to provide a fast, informative string to be incorporated into
you're shell prompt. With this in mind `git-prompt` is written in C and uses
the `libgit2` library, enabling robust, visible, repository status discovery
without slowing down you're shell.

## Installation

To install `git-prompt` for use from the command line or, most likely, for
integration with you terminal prompt, run the CMake `install` target. This will
add the `git-prompt` executable and the `libgit2` library to the
`CMAKE_INSTALL_PREFIX` location.

The ability to compile and link against the system version of `libgit2` has yet
to be added.

## Options

To view the current set of options `git-prompt` can be invoked from the command
line using the `-h` or `--help` flags. At the time of writing the following
output is produced.

```
Usage: git-prompt <options>

Options:
    -h --help         Show this help dialogue
    --submodules      Enable submodule status updates
    --debug           Enable debug output
    prefix "("        Change the prefix token to '('
    suffix ")"        Change the suffix token to ')'
    separator "|"     Change the separator token to '|'
    staged "●"        Change the staged token to '●'
    conflicts "×"     Change the conflicts token to '×'
    changed "+"       Change the changed token to '+'
    clean "✓"         Change the clean token to '✓'
    untracked "…"     Change the untracked token to '…'
    ahead "↑"         Change the ahead token to '↑'
    behind "↓"        Change the behind token to '↓')"
```

## Usage

At the time of writing the following configurations have only been test with
zsh on OS X, however there should be no reason that `git-prompt` would not work
equally as well on Linux or with another shell, as long as the appropriate
escape codes are used to maintain a correct prompt length.

The following is a basic example of using `git-prompt` with the default
settings which aim to be relatively sane.

```zsh
git_prompt() {
  prompt=`git-prompt`
  echo "$prompt"
}

PS1='$(git_prompt) '
```

Here's an example of my prompt which adds color, adding extra meaning, to most
elements of the `git-prompt` output.

```zsh
git_prompt() {
  echo `git-prompt \
    prefix '' \
    suffix '%{\e[1;0m%}' \
    branch '%{\e[1;35m%}' \
    separator ' ' \
    staged '%{\e[0;32m%}*' \
    conflicts '%{\e[1;31m%}×' \
    changed '%{\e[0;31m%}+' \
    clean '%{\e[0;32m%}✓' \
    untracked '%{\e[0;31m%}…' \
    ahead '%{\e[1;0m%}↑' \
    behind '%{\e[1;0m%}↓'`
}

PS1='[%{$fg_bold[white]%}%D{%H:%M:%S}%{$reset_color%}] %{$fg_bold[green]%}%n%{${reset_color}%} «%{$fg_bold[blue]%}%~%{$reset_color%} $(git_prompt)» '
```
