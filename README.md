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

<p>
  Provides offline speech recognition features for the Freeswitch. <br>
  Was developed just as a gizmo to checking/playing with the Whisper capabilities. <br>
  Not suitable for production use in this view! (see <a href="https://github.com/akscf/whisperd">whisperd</a> for that case) <br>
  <br>
   
  <strong>Pay attention on the following: </strong> <br>
  Seems Freeeswith (or APR) has a trouble with dynamic modules that contains c++ code (haven't investigate it yet deeply). <br>
  After the first loading you can longer unload it completely! (something still continue keeping it somewhere) <br>
  It can be repeated (for instance, take the mod_whisper_asr): <br>
   - load mod_whisper_asr <br>
   - unload mod_whisper_asr <br>
   - now, go to the Freeswitch modules dir and delete/move this module away <br>
   - the following attempt to load this unavailable module will complete with success! <br>
     there is an only way to solve it - it's reload the Freeswitch <br>

  Got it on: FreeSWITCH Version 1.10.10-release~64bit, but haven't tested on other versions, <br>
  maybe it's an old and well known bug but I stumbled upon it for the first time in this module (never used c++ with Freeswitch) and I takes me plenty of time to figure out what's going on...<br>

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
