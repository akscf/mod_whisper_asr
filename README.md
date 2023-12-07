<p>
  Provides offline speech recognition features for the Freeswitch. <br>
  Was developed just as a gizmo to checking/playing with the Whisper capabilities. <br>
  Not suitable for production use in this view! (see <a href="https://github.com/akscf/whisperd">whisperd</a> for that case) 
</p>

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
