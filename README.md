# M5Cardputer FrogFind Browser

A simple C++/Arduino sketch for `M5Cardputer` that:

- uses `FrogFind` as the owner,
- renders the page as plain text,
- links are numbered,
- allows you to open a link, e.g., create a Wikipedia page,
- has a Wi-Fi start screen with a list of networks and password saving.

## How it works

This is not a full graphical HTML browser.
`M5Cardputer` downloads a simplified page from `FrogFind`, extracts the text and link, and then displays it on the screen in a lightweight, readable format.

This allows:

- text to be more readable on a small screen,
- further links,
- Wikipedia and the website have more meaning than in raw HTML,
- no need to hard-code SSIDs and passwords.

## Start Wi-Fi

After starting the device:

- scans for available Wi-Fi networks,
- refreshes the list automatically every few seconds,
- sorts networks first by strength, then by name,
- allows you to select a network by entering its number,
- then proceeds to the password entry screen.

In the list:

- `****` indicates a very strong signal,
- `O` indicates an open bandwidth,
- `P` indicates a password-protected network.

## Control

- On the Wi-Fi screen, enter the network number + `ENTER`
- On the password screen, enter the password + `ENTER`
- On the password screen, enter `b` + `ENTER` = return to the Wi-Fi list
- On the list screen, enter `ENTER` without text = manual refresh
- On the list screen, enter `r` + `ENTER` = manual refresh
- `;` and `.` keys = scroll through the Wi-Fi list or page
- Enter plain text + `ENTER` = search with `FrogFind`
- Enter the link number + `ENTER` = open the link
- Enter `b` + `ENTER` = back
- Enter `r` + `ENTER` = refresh
- Enter `u wikipedia.org` + `ENTER` = open the address through the `FrogFind` proxy
- Button `G0` = fast page refresh

## Libraries

In the Arduino IDE, install:

- `cartputer M5`
- package Informational `M5Stack`

Table:

- `cartputer M5`

## Limitations

- This is a lightweight text browser, not a full browser.
- JavaScript-independent pages are still not well secured.
- Some links may require the HTML parser to be refined.
- Wi-Fi scanning is cyclical and briefly locks the interface while lists are being extended.