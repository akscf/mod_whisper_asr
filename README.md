<p>
  Provides offline speech recognition features for the Freeswitch based on whisper_cpp. <br>
</p>

## versin 1.0 
 Was capable to work with the first versions of whisper_cpp (not longer supported)

## versin 1.1_12062024
 Capable to work with the latest version whisper_cpp (revelant on the date: 12.06.2024) <br>
 Before compile the module, you should build the whisper_cpp as a shared library and (possible) correct it's paths in the Makefile. <br>
 <b>The issues:</b> unfortunately i don't have enough time (now) to deeply dig what is going on, mb it's my mistake somewhere there. <br>
 But have the follogin (although the same time the whisperd works well):
```txt
2024-06-12 07:34:38.686528 99.73% [NOTICE] utils.c:115 transcribe samples=31360
terminate called after throwing an instance of 'std::length_error'
  what():  cannot create std::vector larger than max_size()
  Aborted (core dumped) ./bin/freeswitch -nf -nonat -nonatmap -nort
```

### Usage example
```xml
<extension name="asr-tets">
 <condition field="destination_number" expression="^(3222)$">
    <action application="answer"/>
    <action application="sleep" data="1000"/>
    <action application="play_and_detect_speech" data="/tmp/test2.wav detect:whisper {lang=en}"/>
    <action application="log" data="CRIT SPEECH_RESULT=${detect_speech_result}"/>
    <action application="sleep" data="1000"/>
    <action application="hangup"/>
 </condition>
</extension>

```

```sh
# using freeswitch packages
apt update ; apt install libfreeswitch-dev libfreeswitch1 libfreeswitch1-dbg
cd mod_whisper_asr/sources
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j
make install
```

