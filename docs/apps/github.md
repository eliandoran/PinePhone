# GitHub

GitHub shows your [GitHub](https://github.com) notifications on the phone, much faster than
the website: review requests, mentions, issues and releases of the repositories you follow.
You can mark them read, and open pull requests to read their conversation and comment. The
phone needs internet (Wi-Fi or mobile data).

GitHub is not part of the firmware: it is installed separately, and then shows up in the menu
under Tools.

## Signing in

GitHub signs in with a **personal access token**, a password made for one app that you can
revoke at any time. Make one on a computer first:

1. On github.com, open **Settings › Developer settings › Personal access tokens › Tokens
   (classic)** and choose **Generate new token (classic)**.
2. Tick the **notifications** scope, and **repo** to also see notifications from private
   repositories.
3. Generate it and keep the page open: GitHub shows the token only once.

Then on the phone:

1. Open **GitHub** from the menu. It shows **Sign in to GitHub**.
2. Tap the **Token** field and type the token (it starts with `ghp_`).
3. Tap **Sign in**, or press Enter on the keyboard.

The phone remembers the token. If GitHub refuses it later (it expired or you revoked it), the
app asks for a new one and says **GitHub refused this token.**

!!! note
    Fine-grained tokens do not work: GitHub only gives notifications to classic tokens.

## Reading notifications

The list shows your inbox as on github.com, newest first, the ones you have read too. Each
notification shows:

- the repository and when it last changed (the time today, the day this week, the date
  before),
- the title of the pull request, issue, release or discussion,
- what it is and why you got it: **Pull request · Review requested**, **Issue · Mentioned**,
  **Release · Subscribed**…

A blue dot marks the unread ones. Tap one to mark it read on GitHub; to mark them all read,
tap the :fontawesome-solid-check: check mark at the top right.

The list loads each time you open GitHub; to load it again, tap the
:fontawesome-solid-arrows-rotate: circular arrows. Without internet, the app keeps showing what
it loaded last, or says **Cannot reach GitHub**. **No notifications** means your inbox is
empty.

## Pull requests

Tap a pull request's notification to open it (this also marks it read). At the top:

- its title and its key, such as **elian/pinephone#7** (the repository and the number),
- its state: **Open**, **Draft**, **Merged** or **Closed**,
- the branches: **Into main from github-app** (the branch it merges into, then the branch with
  the changes).

Below it, the conversation, as in Messages: the description first, then the comments and
reviews in the order they came. Each comment shows who wrote it and when; yours are on the
right in blue. A review shows as a line, such as **alice approved** or **bob requested
changes**, followed by its text if it has any. Each message is one bubble (a very long one is
cut short), with its formatting: headings, **bold**, *italic*, `code`, links (blue,
underlined), lists and task lists (a box, ticked when done), quotes (a bar on the left), code
blocks (a darker box: swipe sideways to see long lines) and lines between sections. Line
breaks stay where they were written, as on github.com.

Messages with HTML or tables, such as the reports bots post, show as GitHub lays them out:
tables, colors and pictures (PNG and JPEG; other pictures, such as most badges, are left out).
Their links cannot be opened yet, and a very long one is cut short.

To comment, tap the **Comment** field at the bottom, type, then tap the
:fontawesome-solid-chevron-up: arrow. Your comment shows **Sending...** until GitHub has it.
If GitHub refuses it, **Not sent** says why, and your text goes back into the field so you can
try again.

## Limitations

- The latest 50 notifications only. Only pull requests open: issues, releases and discussions
  do not yet.
- In a pull request, no changed files, commits or comments on lines of code yet, and no
  approving, merging or closing.
- No **Done**, saving or unsubscribing: do these on the website.
- The list does not refresh by itself, and the phone does not tell you about new notifications.
