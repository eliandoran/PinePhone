# Email

Email reads and sends the mail of your account over IMAP and SMTP, the protocols every mail
provider offers. The phone needs internet (Wi-Fi or mobile data).

Email is not part of the firmware: it is [installed separately](index.md#installing-an-app), and
then shows up in the menu under **Essentials**.

!!! note
    So far the app only sets up the account. Reading and sending mail come next.

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

## Removing the account

Open **Email**, tap **Remove account**, then **Remove** in the bar that appears (or **Cancel**).
The form shows again, for another account.
