param([string]$Out)
Add-Type -AssemblyName System.Speech
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(48000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$voices = $synth.GetInstalledVoices() | Where-Object { $_.Enabled } | ForEach-Object { $_.VoiceInfo.Name }

$commands = @(
  "Jarvis, set my game to Hades.",
  "Jarvis. Clip that.",
  "Jarvis, start a poll. Which boss is next?",
  "Jarvis, change the title to chill Sunday stream.",
  "Jarvis, run an ad.",
  "Jarvis, marker. That was a great play."
)
$monologue = "Okay so here is the thing about this boss, you have to wait for the second phase and then dodge to the left every single time he raises the hammer, " +
  "because if you go right you get pinned against the wall and there is nothing you can do about it, and honestly I have died to that maybe forty times tonight, " +
  "so chat if you have any better ideas I am all ears, because I am running out of patience and potions and the shop does not restock until the next floor, " +
  "which means we either push through right now or we go back and grind the first area again for another twenty minutes and nobody wants to watch that."

$i = 0
foreach ($voice in $voices) {
  $synth.SelectVoice($voice)
  $synth.Rate = 0
  $synth.SetOutputToWaveFile("$Out\talk-$i.wav", $format)
  foreach ($c in $commands) { $synth.Speak($c) }
  $synth.SetOutputToNull()
  $synth.Rate = 2
  $synth.SetOutputToWaveFile("$Out\monologue-$i.wav", $format)
  $synth.Speak($monologue)
  $synth.SetOutputToNull()
  $i++
}
"voices: $($voices -join ', ')"
