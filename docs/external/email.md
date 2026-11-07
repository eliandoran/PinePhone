# Email

Email reads and sends the mail of your account over IMAP and SMTP, the protocols every mail
provider offers. The phone needs internet (Wi-Fi or mobile data).

Email is not part of the firmware: it is [installed separately](index.md#installing-an-app), and
then shows up in the menu under **Essentials**.

!!! note
    So far the app sets up the account, lists the inbox and opens its messages. Other
    folders, attachments and sending come next.

## Signing in

1. Open **Email** from the menu. It shows **Your email account**.
2. Tap **Email address** and type your address, then **Password** and type its password.
3. Tap **Sign in**, or press Enter on the keyboard. Enter moves on to what is still missing.

The phone looks up your provider's mail servers in Thunderbird's database, which knows most
of them, tries the password on the IMAP server, and keeps the account. If the provider is not
in the database, the form asks for its **IMAP server** and **SMTP server**: the host name
each (`mail.example.org`), with a port after a colon when it is not the usual one (993 for
IMAP, 465 for SMTP, both over TLS).

If the server refuses the password, the form says so and keeps what you typed; **Cannot
reach** a server means the phone has no internet or the host name is wrong. **The certificate
of … cannot be trusted** means the server proves another name than the one you typed (a mail
server often answers for several domains but holds a certificate for one: use that name, the
one your provider gives in its settings), or a certificate the phone does not know the issuer
of; the phone never signs in to such a server.

!!! note
    Providers that only allow signing in through their website (Gmail and Outlook.com, with
    OAuth) need an **app password** made in the account's security settings, with two-step
    verification turned on. Type that instead of the account's password.

The address and password are kept on the phone, on the data partition, and stay across
updates of the firmware.

## The inbox

Once signed in, **Email** opens on the inbox: its last 50 messages, newest first, each with
the sender, when it arrived and its subject. Unread messages show their time in blue and
their subject in full color. The list is loaded when the app opens and when you pull it down
from the top; what was loaded last is kept and shown at once the next time, offline too, with
a line above it saying why nothing new came (**Cannot reach …**).

## Reading a message

Tap a message: its subject, sender and date come first, then its text. A message written
as a web page (most newsletters and notifications are) is laid out as one, with its images
loaded from the internet; in the dark theme it is drawn dark, as Web Lite's pages and Books'
pages are (its white the card's color, its colors as light as they were dark). One written as
plain text is shown as such.
Only the part you read is downloaded, not the attachments. Opening a message marks it read on
the server too, so other mail programs see it read.

What you have read is kept on the phone and opens again at once, offline too; a message that
leaves the inbox's last 50 takes its kept copy with it.

## Removing the account

Tap :fontawesome-solid-user: in the header: the account's screen shows the address and its
server. Tap **Remove account**, then **Remove** in the bar that appears (or **Cancel**). The
account and the messages kept on the phone go, and the form shows again, for another account.
