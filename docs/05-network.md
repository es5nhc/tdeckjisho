# 05 – Network

| Feature | Where |
|---|---|
| Wi-Fi: scan, connect, the status bar | `hal/tdeck/Net.cpp`, `screens/WifiScreen.*` |
| Saved networks and passwords | `core/WifiStore.*`, `hal::secretRead/Write` |
| One page or feed into memory (cancellable, with progress) | `hal::Fetch`, `screens/WebLoadScreen.hpp` |
| A big file straight to the card (not blocking) | `hal::Download` |
| What may be fetched at all | `core/Url.*`: `isWebUrl()`, `redirectProblem()` |
| HTTPS certificates | `tools/make_ca_bundle.py`, `hal/tdeck/CaVerify.cpp` |
| Feeds, the web reader | `screens/Actions.cpp`, `text/Feed.cpp`, `text/Html.cpp` |

In the simulator the same functions are implemented with libcurl
(`hal/sdl/Net.cpp`). Wi-Fi there is pretend: "connecting" succeeds after a moment, and a
scan finds two made-up networks.

## Wi-Fi

The ESP32's Wi-Fi runs in ESP-IDF's own background tasks. `WiFi.begin()` returns at
once, and `WiFi.status()` reports later whether it worked. `hal::wifiState()` turns that
into four states, `Off`, `Connecting`, `Connected` and `Failed`, and gives up after 20 s
(`kConnectTimeoutMs`). The status bar shows `WiFi`, `WiFi…` or `WiFi×`.

When the connection first succeeds, `wifiState()` also starts **NTP** (the network
clock). From then on ESP-IDF keeps the clock in sync by itself.

Scanning takes 2–4 seconds, so it uses the start-then-poll pattern from chapter 01:
`wifiScanStart()`, then `wifiScanPoll()` every frame until it returns something other
than -1.

The ESP32 cannot scan while it is still trying to *join* a network: `scanNetworks()`
fails at once. So `wifiScanStart()` first drops a connection attempt in progress
(`Connecting` or `Failed` become `Off`); scanning while `Connected` works and is left alone.

### Auto-connect: whichever saved network is here

At boot, `App::begin()` calls `App::startAutoConnect()`. Instead of blindly trying the
most recently used network (useless when you are at a café, not at home), it starts a
scan; `App::pollAutoConnect()` waits for the result and joins the saved network with the
strongest signal (`pickSavedWifi()` in `src/core/WifiStore.cpp`, unit-tested on the PC).
If none of them is in range, the radio goes off straight away.

The same happens once after a lost connection: when the driver's reconnect attempts end
in `Failed` (you walked away from the router), the app scans for another saved network.
Only once until the next successful connection (`_autoUsed`): a saved network that is in
range but refuses us, say after a password change, must not cause an endless
scan-connect loop. Neither runs while the Wi-Fi screen is open; there you are choosing.

### Switching the radio off when there is no network

`Failed` is only what the status bar shows. With auto-reconnect on, the driver keeps
looking for the network forever (you left home, the router is off), and the radio is one
of the biggest power users on the board. So `App::checkWifiGiveUp()` (`src/ui/App.cpp`)
starts a clock each time the state turns to `Connecting`: at boot, after a lost
connection, or after you pick a network. If the state hasn't reached `Connected` within
`wifi_off_s` seconds (default 60, Home → Settings), the app calls `hal::wifiDisconnect()`,
which turns the radio off, and shows a toast. Settings → Wi-Fi turns it on again. The check
also runs while the screen is off.

While the Wi-Fi screen is open the clock is held (the screen's `holdsWifi()` returns
true), so the radio is never pulled out from under a scan you are watching.

### Passwords never go on the SD card

Anyone who borrows the card can read a text file on it in ten seconds. Saved networks
are therefore kept in the ESP32's **NVS** ("non-volatile storage", a small key-value
store in its own flash partition), through `hal::secretWrite("wifi", …)`. Inside it's
one network per line, the name and password separated by a TAB, most recently used
first, at most 8 (`kMaxWifiNetworks`). A name or password containing a TAB, a line
break or another control byte is refused, because it can't be stored safely in that
format.

**Setting up without typing:** put the same lines into
`/tdeckjisho/config/wifi.txt` (see `sdcard/tdeckjisho/config/wifi.txt.example`). At the
next start the app moves them into NVS and overwrites the file, so the password
doesn't stay on the card.

NVS isn't **encrypted**. Someone with the device and a USB cable can still read the
flash. Protecting against that needs flash encryption, which doesn't combine with
Launcher.

In the simulator, secrets go into `./.jisho-secrets/` (or `$JISHO_SECRETS`), a folder
only your user can open. It's outside the simulated SD card on purpose.

## The rules for fetching

`src/core/Url.cpp` holds the network policy for both targets, in one place:

- **Only `http://` and `https://`.** No `file:`, `ftp:` or anything else, and no control
  bytes anywhere in the address. A CR LF could inject an HTTP header.
- **Redirects are followed by hand**, at most 5, and every hop is checked against the
  same rule. A server could otherwise redirect to `file:///…`.
- **Never from `https` back to `http`.** After such a downgrade, anyone on the network
  could read and answer the next request.
- **Certificates are always checked.** There's no "insecure" fallback. If the device
  can't check a server, it refuses the page.
- **Every download has a size limit:** 1.5 MB for a page or feed, 128 MB for
  `hal::Download` (`kMaxBytes`). A server can claim any size, and the limit keeps it
  from filling the RAM or the card.

`HTTPClient` can follow redirects itself, but not from `http://` to `https://`, because
that needs a different client object. Following them by hand also gives the final URL,
which relative links in a page need.

## `Fetch` and `Download`

Both use the start-then-poll pattern (chapter 01), and both follow redirects through the
same function, `beginGet()` in `hal/tdeck/Net.cpp`.

`hal::Fetch` loads a page or a feed into a `std::string`, at most `maxBytes`.
`start()` connects. On the device that **blocks**: `HTTPClient::GET()` does the DNS
lookup, TCP, the TLS handshake and waits for the response headers in one call, and none
of that can be split up. It usually takes 0.5–3 s, at most the 10 s timeout. After that,
each `poll()` reads only what has already arrived, for at most about 25 ms. On a slow
link the body is the long part, and that part can be cancelled.

There is one trick. `Fetch` asks for **HTTP/1.0**. An HTTP/1.1 server may send the body
*chunked*: pieces with a size line before each one, so the server needn't know the
total in advance. `HTTPClient`'s blocking `writeToStream()` decodes that, but a raw
read of the socket would see the size lines mixed into the page. An HTTP/1.0 server
never sends chunks: just the body, then it closes the connection. "Connection closed"
therefore means "done", unless the server promised a `Content-Length` and sent less,
which is an error.

`screens/WebLoadScreen.hpp` drives it. It shows "Connecting to …" before the blocking
`start()` (the same draw-first trick as `LoadingScreen`), then the byte count, with a
bar when the server sent `Content-Length`. Backspace cancels. When the body is complete,
a callback turns it into a screen (the web reader or a feed list), and `WebLoadScreen`
replaces itself with that screen.

In the simulator, `Fetch` uses libcurl's *multi* interface, so there even the connection
doesn't block. To watch the counter on a fast PC connection, set `JISHO_NET_KBPS=50`.

`hal::Download` is for files far bigger than RAM, such as JMdict (10 MB compressed). It
works the same way, but writes to `<path>.part` on the card instead of to memory. The
file is renamed to its real name only when every byte has arrived, so a broken download
never looks finished. It asks with HTTP/1.1, and the server must send `Content-Length`.

## HTTPS: how the device decides whom to trust

TLS encrypts the connection, but encryption is worthless if you don't know **who** is
on the other end: anyone on the café Wi-Fi could answer instead of the real server. The
server proves who it is with a **chain of certificates**:

```
news.err.ee   ← signed by WE1   ← signed by GTS Root R4   (a root certificate you trust)
```

Each certificate is signed by the one to its right. The chain is accepted only if it
ends at a **root certificate** the device already trusts. The trusted list is Mozilla's,
the same one Firefox and most Linux systems use.

### From the PEM file to the firmware

1. `tools/bundle/cacert.pem` is your operating system's copy of the list, for example
   `cp /etc/ssl/certs/ca-certificates.crt tools/bundle/cacert.pem`. It's git-ignored:
   copied from your OS, not ours to redistribute.
2. Before every device build, `tools/make_ca_bundle.py` keeps only each root's **name
   and public key** (all a check needs), sorts them by name, and writes them into
   `.pio/build/tdeck/generated/ca_bundle.h` as a byte array, `kCaBundle`. The build log
   says `make_ca_bundle: 120 root certificates, 55354 bytes`. The header is rewritten
   only when the PEM changed, and a changed header recompiles `Net.cpp`.
3. The array is a `static const`, so it stays in flash (about 54 KB) and costs no RAM.
4. For every HTTPS connection, `Net.cpp` passes it to
   `WiFiClientSecure::setCACertBundle()`.

Without the PEM, the bundle is empty, the build prints a loud warning, and the firmware
refuses every `https://` address with "no CA certificates in this build".

To pick up new roots (Mozilla's list changes a few times a year), copy the PEM again
and rebuild.

### Cross-signed roots, and why `CaVerify.cpp` exists

mbedTLS (the TLS library in ESP-IDF) walks the chain the server sends and calls a
**verify callback** for the topmost certificate it couldn't trust on its own. The
Arduino core's callback asks one question: *was this certificate signed by a root in
the bundle?*

Many servers end their chain with a **cross-signed** root, a newer root certificate
signed by an older one, so that old devices which only know the old root still work:

```
err.ee ← WE1 ← "GTS Root R4", signed by the old "GlobalSign Root CA"
```

GTS Root R4 is in Mozilla's list. GlobalSign Root CA has been retired from it, so the
core's answer is "no", and the site fails. A PC doesn't have this problem: OpenSSL
notices that the cross-signed certificate has the same name **and the same public key**
as the trusted GTS Root R4, so it effectively *is* that root, with a different signature
on it.

`src/hal/tdeck/CaVerify.cpp` replaces the core's three bundle functions
(`arduino_esp_crt_bundle_set/attach/detach`) with versions that accept a certificate
if:

1. it has the same name and public key as a root in the bundle (above the site's own
   certificate only), **or**
2. it was signed by a root in the bundle (the core's original rule).

mbedTLS still checks every signature further down the chain, the expiry dates and the
host name itself, so this trusts exactly what a PC trusts and nothing more.

**How the replacement takes effect:** the linker takes a function from the first place
that defines it. Our object files come first, and a library archive is searched only for
what is still missing. All three functions are defined in our code, so the core's
`esp_crt_bundle.c` is never linked in. Defining only some of them would pull it in
anyway and fail with "multiple definition". You can check with

```sh
~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-nm \
    .pio/build/tdeck/tdeckjisho-*.elf | grep esp_crt_verify_callback
```

It should print nothing. The core's callback would show up under that name.

### When HTTPS fails

The message on the screen is `HTTPClient`'s text for its error code, and "connection
refused" (code -1) means *any* failure to connect, a rejected certificate included.
The serial log (chapter 04) says what really happened:

| Log | Meaning |
|---|---|
| `[tls] untrusted certificate (depth 2): no bundle root for issuer …` | the chain ends at a root that isn't in the bundle: the PEM is old or incomplete, or the site uses a private CA |
| `(-12288) X509 - A fatal error occurred` | the line the core logs after the one above |
| `no CA certificates in this build` | `tools/bundle/cacert.pem` was missing when you built |
| `(-9984) X509 - Certificate verification failed` | a check other than the root failed. Often the clock: until NTP has set it, the device thinks it's 1970 and every certificate is "not valid yet". Otherwise an expired certificate or a wrong host name |
| a reboot during a handshake | the loop task's stack (16 KB, `main.cpp`) or internal RAM ran out |

To see which chain a site actually sends, ask OpenSSL on the PC:

```sh
openssl s_client -connect news.err.ee:443 -servername news.err.ee -showcerts </dev/null
```

## Feeds and the web reader

**Feeds** are listed in `/tdeckjisho/config/feeds.txt`, one per line:

```
# comment
ERR News (English) | https://news.err.ee/rss
https://example.org/feed.xml            (no name: the host name is shown)
```

`a` in the feed list adds one. Opening a feed downloads it (`Fetch`), makes sure it's
UTF-8, and `rt::parseFeed()` (`src/text/Feed.cpp`) finds the `<item>` (RSS) or
`<entry>` (Atom) blocks without a full XML parser. An article shows its summary, which
is HTML and goes through `parseHtml()` (chapter 03), plus a link to the full page.

**The web reader** (Home → `w`) is reader mode only: `parseHtml()` keeps the text,
headings, lists and links, and drops scripts, styles and layout. Pictures are shown as
`[alt text]`. A page that turns out to be a feed opens as a feed. The address you typed
last is remembered as `last_url` in `settings.txt`.

Pages are limited to 1.5 MB. A page that was cut off says so at its end.
