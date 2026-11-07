# GitHub

GitHub shows your [GitHub](https://github.com) notifications on the phone, much faster than
the website: review requests, mentions, issues and releases of the repositories you follow.
You can mark them read, open pull requests, issues and discussions to read their
conversation and comment, merge pull requests and set their milestones. It also lists your repositories. The phone needs internet (Wi-Fi
or mobile data).

GitHub is not part of the firmware: it is installed separately, and then shows up in the menu
under Tools.

## Signing in

GitHub signs in with a **personal access token**, a password made for one app that you can
revoke at any time. Make one on a computer first:

1. On github.com, open **Settings › Developer settings › Personal access tokens › Tokens
   (classic)** and choose **Generate new token (classic)**.
2. Tick the **notifications** scope, and **repo** to also see notifications from private
   repositories, open their discussions and comment.
3. Generate it and keep the page open: GitHub shows the token only once.

Then on the phone:

1. Open **GitHub** from the menu. It shows **Sign in to GitHub**.
2. Tap the **Token** field and type the token (it starts with `ghp_`).
3. Tap **Sign in**, or press Enter on the keyboard.

The phone remembers the token. If GitHub refuses it later (it expired or you revoked it), the
app asks for a new one and says **GitHub refused this token.**

!!! note
    Fine-grained tokens do not work: GitHub only gives notifications to classic tokens.

## Repositories and notifications

Once you are signed in, a bar at the bottom switches between two lists: tap
:fontawesome-solid-folder: **Repositories** or :fontawesome-solid-bell: **Notifications**. The
one shown is in blue. GitHub always opens on **Repositories**.

Each list loads the first time you show it after opening GitHub; to load the one shown again,
tap the :fontawesome-solid-arrows-rotate: circular arrows at the top right. Without internet,
the app keeps showing what it loaded last, or says **Cannot reach GitHub**.

## Repositories

**Repositories** lists the repositories you own, the one you pushed to last first (not those
of organizations or other people you work on). Each shows:

- its name and when you last pushed to it,
- its description, if it has one,
- what it is: **Private**, **Fork** or **Archived** when it is one, then its main language and
  its stars, such as **C · ★ 12**.

**No repositories** means your account has none of its own.

Tap a repository to open it: its name, its description, its address on github.com, and how
many stars and forks it has. To go back to the list, tap the :fontawesome-solid-chevron-left:
back arrow.

To read its README, tap **README** at the bottom of the repository. It shows with the same
formatting as comments, across the whole screen. A README with HTML, such as a centered logo or
badges, shows as GitHub lays it out, with its pictures (PNG and JPEG); a very long one is cut
short. **No README** means the repository has none.

To see what is going on in it, tap **Issues** or **Pull requests**. The list shows the open
ones, the newest first, and its title says how many there are (**42 issues**). Each shows its
number, who opened it and when, then its title. Tap one to open it, as from a notification.

To see the closed ones instead, tap **Closed** at the top, and **Open** to go back. A closed
pull request says **Merged** if it was; a closed issue says **Not planned** or **Duplicate** when
it was closed for that reason. An open pull request not ready yet says **Draft**. **No open
issues** or **No closed issues** (or pull requests) means there are none.

A pull request's row also shows how its checks (GitHub Actions and other CI) went, at the right:
:fontawesome-solid-circle-check: in green when they all passed, :fontawesome-solid-circle-xmark: in
red when one failed, :fontawesome-solid-circle: in orange while some are still running. A pull
request without checks shows none.

A list shows 30 at a time. When there are more, a bar at the bottom goes from page to page: tap
a page's number, :fontawesome-solid-chevron-left: or :fontawesome-solid-chevron-right: for the
previous or next page, :fontawesome-solid-angles-left: for the first or
:fontawesome-solid-angles-right: for the last. The page shown is in blue; the bar shows five
numbers around it.

### Bookmarks

To keep a repository at hand, yours or anyone's, open it and tap the
:fontawesome-regular-bookmark: bookmark at the top right; it fills in
(:fontawesome-solid-bookmark:). The repository then shows at the top of **Repositories** under
**Bookmarks**, the newest first, with your own repositories below under **Your repositories**.
Tap a bookmark to open the repository; to remove it, tap the filled bookmark there. Bookmarks
stay on the phone, and show even without internet.

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

**No notifications** means your inbox is empty.

## Pull requests, issues and discussions

Tap the notification of a pull request, an issue or a discussion to open it (this also marks
it read). At the top:

- its title and its key, such as **elian/pinephone#7** (the repository and the number),
- its state: for a pull request **Open**, **Draft**, **Merged** or **Closed**; for an issue
  **Open**, **Closed** (done, in purple) or **Not planned**; for a discussion **Open**,
  **Answered**, **Closed** (resolved, in purple), **Outdated** or **Duplicate**,
- for a pull request, the branches: **Into main from github-app** (the branch it merges into,
  then the branch with the changes); for an issue, its labels in their colors; for a
  discussion, its category, such as **Q&A**.
- for a pull request or an issue, its milestone, such as **Milestone v1.0** (see
  [Milestones](#milestones)).
- for a pull request with checks, how they went: **1 check failed**, **2 checks pending** or
  **All checks passed**. Tap it to see each check by name, the failed first, then the running,
  passed and :fontawesome-solid-circle-minus: skipped or cancelled ones; tap it again to fold them.
- for an open pull request in a repository you can push to, the green **Merge pull request**
  button (see [Merging a pull request](#merging-a-pull-request)).

To see the repository of a pull request or an issue, tap the :fontawesome-solid-folder: folder
at the top right. It opens the repository as in **Repositories**, with its README, whoever owns
it. **Repository not found** means GitHub does not show it to you (it is private, or was
deleted).

Below the title and state, the conversation, as in Messages: the description first, then the
comments and a pull request's reviews in the order they came. Each comment shows who wrote it and when;
yours are on the right in blue. A review shows as a line, such as **alice approved** or **bob requested
changes**, followed by its text if it has any. In a discussion, the replies to a comment come
right after it, set in a little and saying whose comment they answer (**bob, replying to
alice**), and the comment chosen as the answer has :fontawesome-solid-check: **Answer** above
it. Each message is one bubble (a very long one is
cut short), with its formatting: headings, **bold**, *italic*, `code`, links (blue,
underlined), lists and task lists (a box, ticked when done), quotes (a bar on the left), code
blocks (a darker box: swipe sideways to see long lines), tables (a grid, the header in
bold; swipe sideways when wider than the bubble) and lines between sections. Line
breaks stay where they were written, and emoji names such as `:white_check_mark:` show as the
emoji (✅), as on github.com.

Messages with HTML, such as the reports bots post, show as GitHub lays them out:
tables, colors and pictures (PNG and JPEG; other pictures, such as most badges, are left out).
Their links cannot be opened yet, and a very long one is cut short.

To comment, tap the **Comment** field at the bottom, type, then tap the
:fontawesome-solid-chevron-up: arrow. Your comment shows **Sending...** until GitHub has it.
If GitHub refuses it, **Not sent** says why, and your text goes back into the field so you can
try again. In a discussion, a comment goes at the end, not as a reply to another comment.

### Merging a pull request

When a pull request is open (not a draft) and you can push to its repository, its card shows
a green **Merge pull request** button. Tap it, then choose how to merge, with GitHub's names:
**Create a merge commit**, **Squash and merge** or **Rebase and merge**. Only the ways the
repository allows show. Nothing is merged until you choose one; **Cancel** puts the button
back.

The card says **Merging...**, then the pull request shows **Merged**. The phone merges the
changes it showed you: if someone pushed to the branch since you opened the pull request,
GitHub refuses. If GitHub refuses (checks that must pass failed, a review is required, the
branch changed), **Not merged** says why, and the button is back to try again once it is
fixed. When the branch has conflicts with the one it goes into, the card says
:fontawesome-solid-circle-xmark: **This branch has conflicts** instead of the button: solve
them on a computer.

The token needs the **repo** scope to merge.

### Milestones

A pull request's or an issue's milestone shows in its card, under its state. When you can
triage its repository (you can push to it, or were given the triage role), the line also shows
when there is none (**No milestone**), with a :fontawesome-solid-chevron-right: arrow: tap it to
choose another milestone. The list shows the repository's open milestones, the soonest due
first, each with its due date (**Due 30 Nov 2026**) if it has one; the current one is ticked.
Tap one to set it, or **No milestone** to remove it. **No open milestones** means the
repository has none to choose from.

Back on the pull request or issue, the milestone you chose shows dimmed until GitHub has it. If
GitHub refuses, **Milestone not set** says why and the milestone stays as it was.

GitHub's notification does not say which discussion it is about, so the app looks for the
latest discussion of that repository with the same title. **Discussion not found** means
there is none: it was deleted or renamed, or it is in a private repository and the token
lacks the **repo** scope.

## Limitations

- The latest 50 notifications only. Only pull requests, issues and discussions open: releases
  and the rest do not yet.
- In a discussion, the first 100 comments and the first 100 replies to each; no replying to a
  comment, no reactions or polls.
- In a pull request, no changed files, commits or comments on lines of code yet, and no
  approving or closing. Merging cannot change the commit's message or delete the branch
  afterwards.
- Of milestones, the 100 open ones due soonest; no creating one, nor choosing a closed one.
- No **Done**, saving or unsubscribing: do these on the website.
- The 100 repositories you pushed to last only. A repository shows only its name, description,
  address, stars, forks, README, and its issues and pull requests: no files, and the links in it
  cannot be opened.
- Of a repository's open or closed issues or pull requests, the newest 1000 only (GitHub's
  search gives no more); one opened or closed a moment ago may take a minute to show.
- The lists do not refresh by themselves, and the phone does not tell you about new notifications.
