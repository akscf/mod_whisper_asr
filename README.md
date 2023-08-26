<p>
  Provives speech recognition features for Freeswitch. Based on the OpenAI Whisper and it's C++ implementation.
  NOTE: Not recommended to use it in production, yet.
</p>

### Features
 - Voice Activity Detection (VAD)
 - relative flexible settings

### ToDo
 - increase performance and decrease resource requirements...
 - improve speech detection a chunk creation to use a parallel mode more efficiently (there are some ideas)


### Dialplan examples
```
<extension name="asr-tets">
 <condition field="destination_number" expression="^(3222)$">
    <action application="answer"/>
    <action application="sleep" data="1000"/>
    <action application="play_and_detect_speech" data="/tmp/test2.wav detect:whisper {lang=en}"/>.
    <action application="log" data="CRIT SPEECH_RESULT=${detect_speech_result}"/>
    <action application="sleep" data="1000"/>
    <action application="hangup"/>
 </condition>
</extension>

```