<p align="right">
  <a href="single-clip-online-pronunciation.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# One clip at a time: why pronunciation is online, and how little of the player that needs

Recorded while building the [English Dict app](english-dict/README.md). A word book without
audio is a word book you cannot hear, so pronunciation was a hard requirement — and the
arithmetic rules out storing it. This entry is the capacity sum that forces the feature
online, and the deliberate decision to take only a small slice of an existing, much larger
player.

## The capacity sum, first

Storing the audio for all 19,513 entries does not fit, by two orders of magnitude:

| Encoding | Per entry | Whole bank |
| --- | --- | --- |
| 48 kHz mono, 67 kbps MP3 | ~9–16 KB | 200 MB+ |
| 8 kHz ADPCM, 4 bit | **4.8 KB** | **~94 MB** |

The flash is 8 MB and roughly 7.9 MB of it can go to content. The compressed-to-4.8-KB
column is already a quality floor nobody would ship, and it is still **12× too big**. So the
decision is not "online is nicer"; it is the only option, and the cost — the device must be
on Wi-Fi — is a cost the sibling applications already pay and already have a provisioning
flow for.

## What is actually fetched

One short clip per word, from Youdao's public endpoint:

```
http://dict.youdao.com/dictvoice?type=0&audio=<word>   (type=0 = US pronunciation)
```

A clip is 1–2 seconds, about 10 KB of MP3. The path is HTTP → `esp_audio_simple_dec`
(software MP3) → I2S, and the buffer that holds a clip is a **static ~9 KB**, allocated
once at init and never freed. Pronunciation is the core interaction of this app and may
fire several hundred times a day; allocating and freeing 9 KB that often would fragment the
heap for no benefit, and unlike the radio sibling there is no moment where the provisioning
portal needs that memory back.

## Take four things from the big player, and leave the rest

The sibling radio application has a 1290-line player. Nearly all of it exists to solve
problems specific to streaming a 20-minute lesson: segmented prefetch, a 16 KB ring buffer,
resuming a partial fetch, per-segment backoff, preemption when the station changes. Dropped
onto a 10 KB, 1.2-second clip, those mechanisms are not "extra safety" — they are a pile of
failure modes that cannot happen, each with code to maintain.

What is kept is exactly four things:

1. **The decode chain itself** — HTTP → `esp_audio_simple_dec` → I2S.
2. **`esp_http_client_is_complete_data_received()` plus `Content-Length`** to tell "played
   to the end" from "the stream died". Reading a zero from the socket looks identical in
   both cases and the two need opposite follow-up, which is why this is a kept judgement
   rather than an optimisation.
3. **An output watchdog.** The hardest field failure is "the UI says playing and the
   speaker is silent" — no log line, the sound just stops. The player pins the time of the
   last successful I2S write and fails the playback with a logged reason if that goes stale.
4. **"A failed write must log."** The first version of the sibling's player did a silent
   `continue`; the sound vanished and the log was empty.

## Three things added, because a key-driven device needs them

- **Request debounce.** The three buttons are ADC-divided onto one GPIO, so contact bounce
  and a user double-tap look the same in software — and every request is a full HTTP
  round trip. Two requests for the same word inside 400 ms collapse into one.
- **Cancellation.** Turning the card over, going back or moving to another word must be able
  to abandon an in-flight network read immediately, rather than wait up to four seconds for
  it to time out while the UI sits frozen.
- **ID3v2 stripping.** The endpoint's MP3 starts with `ID3\x04` (ID3v2.4); handed straight
  to the decoder it desynchronises for a stretch. The header length lives in a syncsafe
  integer, so removing it is more reliable than letting the decoder discover it.

## State, and what the UI is told

A single playback task handles one word at a time, and reports a five-value state —
`IDLE`, `LOADING`, `PLAYING`, `DONE`, `FAILED` — so the word page can draw a two-to-four
character note (loading / playing / failed) next to the IPA. `wd_audio_play_word` returns
`false` for conditions that are **not errors** — offline, empty word, the same word already
playing, or a request inside the 400 ms window — and the UI stays silent for all of them,
because none of them is something the reader needs to be told about.

Changing word preempts: the previous clip is abandoned rather than allowed to finish, since
the reader has already moved on and hearing the old word finish would be confusing rather
than helpful.

## Related

- [English Dict app](english-dict/README.md) — the application, including the word page that
  shows this state.
- [Turning a word book into a flash blob](word-book-to-flash-blob.md) — the other half of
  the same budget: what does fit in flash, and how it is stored.
- [Letting measured line height, not font size, decide the layout](measured-line-height-over-font-size.md)
  — where the one-to-four-character audio note has to fit.
