Host-side tests for the audio decoders and metadata parsers.

The Zephyr fs/log APIs are shimmed over stdio (shim/), so flac.c and
metadata.c compile unchanged on any Linux/macOS host with gcc.

Build:
    cd host-test
    gcc -O2 -I shim -I ../src -I ../lib/helix/pub decode_test.c \
        ../src/decoder.c ../src/flac.c shim/mp3_stub.c -o decode_test
    gcc -O2 -I shim -I ../src metadata_test.c shim/sd_card_stub.c \
        ../src/metadata.c -o metadata_test

Decode test (bit-exact against a reference decoder):
    ffmpeg -i any.flac -f s16le -acodec pcm_s16le -ac 2 ref.pcm
    ./decode_test any.flac out.pcm
    cmp out.pcm ref.pcm && echo PASS

Generate test files:
    ffmpeg -f lavfi -i "sine=frequency=440:duration=3" -ac 2 -f flac t.flac
    flac -s -f -8 -o t_hi8.flac input.wav

Metadata test:
    flac -s -f --tag=TITLE="T" --tag=ARTIST="A" --picture=cover.jpg \
        -o t_tags.flac input.wav
    ./metadata_test

ID3-prefixed FLAC (tag prepended by some taggers) is handled by
decoder_open; generate with a spec-correct 10-byte ID3v2 header.
