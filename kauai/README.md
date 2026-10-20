# Kauai

Things to play Kauai songs with, beyond the instruments Kauai has built in.

`sounds/` holds `external` instruments. Each is a folder: its `main.cul` is
the module that makes the sounds, and the data it plays from lies beside it.
`culebra play --sounds <folder> song.kau` plays a song with one, and a
program imports its `main.cul` and hands Kauai the module's `voices` (and
`drums`, for a kit).

| Instrument | `external` | What it is |
|---|---|---|
| [`sounds/soprano_sax`](sounds/soprano_sax) | `Sax` | a soprano saxophone played from real notes |

`tools/` holds what works with them: [`tools/make_sounds.cul`](tools) makes
every sound songs ask an instrument for beforehand, as files a program plays
in its place.
