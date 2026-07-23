Import("env")
import os, subprocess

base = os.path.join(env["PROJECT_LIBDEPS_DIR"], env["PIOENV"], "ESP32-audioI2S", "src")
audio_cpp = os.path.join(base, "Audio.cpp")
audio_h   = os.path.join(base, "Audio.h")

# ── 2. Audio.h: swapChannels()-Methode + m_swapChannels-Flag hinzufügen ───────
if os.path.exists(audio_h):
    with open(audio_h, "r") as f:
        hsrc = f.read()
    # Idempotent: Anker enthält die Folgezeile, sodass nach Patch kein Match mehr möglich
    hpatched = hsrc.replace(
        "    void forceMono(bool m);\n    void setBalance(int8_t bal = 0);",
        "    void forceMono(bool m);\n    void swapChannels(bool swap);\n    void setBalance(int8_t bal = 0);"
    )
    hpatched = hpatched.replace(
        "    bool            m_f_forceMono = false;          // if true stereo -> mono\n"
        "    bool            m_f_internalDAC = false;",
        "    bool            m_f_forceMono = false;          // if true stereo -> mono\n"
        "    bool            m_swapChannels = false;         // if true swap L/R channels\n"
        "    bool            m_f_internalDAC = false;"
    )
    if hpatched != hsrc:
        with open(audio_h, "w") as f:
            f.write(hpatched)
        print("patch_audio_lib: Audio.h swapChannels() Deklaration + m_swapChannels hinzugefügt.")

# ── 1. ESP32-audioI2S 2.3.0: min()-Typkonflikt mit GCC14 patchen ─────────────
audio_cpp = os.path.join(base, "Audio.cpp")

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
    # swapChannels()-Implementierung nach forceMono() einfügen (Anker: forceMono + setBalance, idempotent)
    patched = patched.replace(
        "void Audio::forceMono(bool m) { // #100 mono option\n    m_f_forceMono = m; // false stereo, true mono\n}\n"
        "//---------------------------------------------------------------------------------------------------------------------\n"
        "void Audio::setBalance",
        "void Audio::forceMono(bool m) { // #100 mono option\n    m_f_forceMono = m; // false stereo, true mono\n}\n"
        "//---------------------------------------------------------------------------------------------------------------------\n"
        "void Audio::swapChannels(bool swap){ m_swapChannels = swap; }\n"
        "//---------------------------------------------------------------------------------------------------------------------\n"
        "void Audio::setBalance"
    )
    # swapChannels()-Logik in Gain(): tauscht LEFT/RIGHT-Samples wenn m_swapChannels gesetzt.
    # Nur für physisch falsch verdrahtete Hardware nötig (Button im Web-UI).
    patched = patched.replace(
        "int32_t Audio::Gain(int16_t s[2]) {\n    int32_t v[2];",
        "int32_t Audio::Gain(int16_t s[2]) {\n"
        "    if(m_swapChannels) { int16_t tmp = s[LEFTCHANNEL]; s[LEFTCHANNEL] = s[RIGHTCHANNEL]; s[RIGHTCHANNEL] = tmp; }\n"
        "    int32_t v[2];"
    )
    # Gain()-Return: ESP32-S3 Legacy-I2S-DMA gibt (LEFT<<16|RIGHT) invertiert aus.
    # Fix: RIGHT in obere 16 Bit, LEFT in untere 16 Bit → korrekte Ausgabe ohne Swap.
    patched = patched.replace(
        "    return (v[LEFTCHANNEL] << 16) | (v[RIGHTCHANNEL] & 0xffff);",
        "    return (v[RIGHTCHANNEL] << 16) | (v[LEFTCHANNEL] & 0xffff);"
    )
    if patched != src:
        with open(audio_cpp, "w") as f:
            f.write(patched)
        print("patch_audio_lib: Audio.cpp min()-Cast + resetBuffer memset + ICY/Chunked + L/R-Swap + Gain-Return gepatcht.")
