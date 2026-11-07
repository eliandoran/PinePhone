# Assistant

Assistant lets you chat with an AI language model: ask a question in your own words and read
the answer as it is written. It uses [DeepSeek](https://platform.deepseek.com)'s models over
the internet (Wi-Fi or mobile data), with your own DeepSeek API key.

![A question answered after a web search](../images/generated/assistant-chat.webp){ width="300" }

!!! note "What is sent"
    Your questions, and the rest of the chat they belong to, are sent to DeepSeek to be
    answered. The chats themselves are kept on the phone.

## Setting it up

Assistant needs an API key from a DeepSeek account. Create one on
[platform.deepseek.com](https://platform.deepseek.com), under **API keys**; what you ask is
paid for by that account.

1. Open **Assistant** from the menu.
2. Tap the :fontawesome-solid-gear: gear at the top right.
3. Tap the **API key** field under **DeepSeek** and type the key.
4. Go back: the key is saved.

![Assistant settings](../images/generated/assistant-settings.webp){ width="300" }

On the same screen:

- **Model**: **deepseek-flash** (fast and cheap, the default) or **deepseek-v4-pro** (smarter,
  slower). Tap one; a :fontawesome-solid-check: check mark shows the chosen one.
- **Thinking**: the model thinks before it answers, for better answers that take much
  longer. Off by default.
- **Web search**: lets the model look things up on the web for recent events and facts it is
  not sure of. Off by default; DeepSeek charges for searches ($0.75 per 1000, as the setting
  says).

## Asking a question

1. In **Assistant**, tap the round :fontawesome-solid-pen: button at the bottom right to start a new
   chat.
2. Type your question in the **Ask anything** field.
3. Press Enter, or tap the :fontawesome-solid-chevron-up: send button next to the field.

Under the answer's bubble, **Waiting...** shows until it starts, then **Writing...** while
the words arrive (**Thinking...** first with Thinking on), and the time once it is done.
Answers are short and in plain text, made for the phone's screen. The model knows today's
date.

With **Web search** on, the model decides when to search. Each search shows between the
messages as **Searched the web:** followed by what it searched for, before the answer.

To ask a follow-up, type it in the same chat: the model sees the whole conversation. Ask one
question at a time; while an answer is on its way, a new one stays in the field.

If you leave a chat while it is being answered, what has arrived so far is kept.

## Your chats

The **Assistant** screen lists your chats, the latest first: each shows its first question,
the last message, and its time (or day). Tap one to read it again or carry on.

![The chats](../images/generated/assistant-chats.webp){ width="300" }

To delete a chat, open it, tap the :fontawesome-solid-trash-can: trash can at the top right,
then **Delete chat** (or **Cancel**).

## When there is no answer

A message under the chat says what went wrong; tap it to try again:

- **No DeepSeek API key: add one in the settings**: set up a key first (the
  :fontawesome-solid-gear: gear in the chat opens the settings too).
- **No internet connection. Tap to try again.**
- An error from DeepSeek, with its code in brackets: 401 means the key is wrong.
- **Not answered. Tap to try again.**: the chat was left without an answer.
