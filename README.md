<p>
  Freeswitch ASR module based on OpenAI Whisper (it's C++ implementation).
</p>

### ToDo
 - improve chunks creation (that should give a positive effect with a parallel mode...)

### Usage example
```
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