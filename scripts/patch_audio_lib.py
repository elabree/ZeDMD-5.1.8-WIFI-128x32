Import("env")
import os, subprocess

# ── 1. ESP32-audioI2S 2.3.0: min()-Typkonflikt mit GCC14 patchen ─────────────
audio_cpp = os.path.join(
    env["PROJECT_LIBDEPS_DIR"],
    env["PIOENV"],
    "ESP32-audioI2S",
    "src",
    "Audio.cpp"
)

if os.path.exists(audio_cpp):
    with open(audio_cpp, "r") as f:
        src = f.read()
    patched = src.replace(
        "availableBytes = min(availableBytes, InBuff.writeSpace());",
        "availableBytes = min(availableBytes, (uint32_t)InBuff.writeSpace());"
    )
    # PSRAM-Buffer beim Senderwechsel leeren: verhindert Altdaten vom vorherigen
    # Stream (der auskommentierte memset in resetBuffer() lässt alte Daten stehen)
    patched = patched.replace(
        "    // memset(m_buffer, 0, m_buffSize); //Clear Inputbuffer",
        "    memset(m_buffer, 0, m_buffSize); //Clear Inputbuffer"
    )
    # ICY-Metadaten + Chunked-Transfer Bug:
    # Wenn m_metacount==0 und readMetadata() aufgerufen wird, wird danach sofort
    # return ausgeführt — das CRLF am Ende des Chunks wird nie gestripped.
    # Das nächste chunkedDataTransfer() liest dann das CRLF als Chunk-Größe und
    # bringt alles durcheinander → alle ~5 Sek. Decode-Fehler + 851× syncword not found.
    patched = patched.replace(
        "        if(m_metacount == 0) {chunkSize -= readMetadata(availableBytes); return;}",
        "        if(m_metacount == 0) {\n"
        "            chunkSize -= readMetadata(availableBytes);\n"
        "            if(m_f_chunked && chunkSize == 0) { stripCRLF(); chunkSize = 0; }\n"
        "            return;\n"
        "        }"
    )
    if patched != src:
        with open(audio_cpp, "w") as f:
            f.write(patched)
        print("patch_audio_lib: Audio.cpp min()-Cast + resetBuffer memset + ICY/Chunked gepatcht.")
