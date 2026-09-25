# Fontconfig rules tracked from omacom/omarchy
# default/fontconfig/conf.avail/50-omarchy.conf.
# Generic families, system UI aliases, and last-resort Arabic coverage.
{
  font,
  serifFont,
  monoFont,
  emoji,
  arabic,
  urdu,
}:
''
  <?xml version="1.0"?>
  <!DOCTYPE fontconfig SYSTEM "fonts.dtd">
  <fontconfig>
    <!-- Ubuntu fonts were removed. A long-running GTK portal can still
         request that family and then fail to open the missing files. -->
    <match target="pattern">
      <test name="family" qual="any">
        <string>Ubuntu</string>
      </test>
      <edit name="family" mode="assign" binding="strong">
        <string>${font}</string>
      </edit>
    </match>
    <match target="pattern">
      <test name="family" qual="any">
        <string>Ubuntu Sans</string>
      </test>
      <edit name="family" mode="assign" binding="strong">
        <string>${font}</string>
      </edit>
    </match>
    <match target="pattern">
      <test name="family" qual="any">
        <string>Ubuntu Condensed</string>
      </test>
      <edit name="family" mode="assign" binding="strong">
        <string>${font}</string>
      </edit>
    </match>
    <match target="pattern">
      <test name="family" qual="any">
        <string>Ubuntu Mono</string>
      </test>
      <edit name="family" mode="assign" binding="strong">
        <string>${monoFont}</string>
      </edit>
    </match>

    <match target="pattern">
      <test name="family" qual="any">
        <string>sans-serif</string>
      </test>
      <edit name="family" mode="assign" binding="strong">
        <string>${font}</string>
      </edit>
    </match>

    <match target="pattern">
      <test name="family" qual="any">
        <string>serif</string>
      </test>
      <edit name="family" mode="assign" binding="strong">
        <string>${serifFont}</string>
      </edit>
    </match>

    <match target="pattern">
      <test name="family" qual="any">
        <string>monospace</string>
      </test>
      <edit name="family" mode="assign" binding="strong">
        <string>${monoFont}</string>
      </edit>
    </match>

    <match target="pattern">
      <test name="lang" compare="contains">
        <string>ar</string>
      </test>
      <edit name="family" mode="prepend" binding="strong">
        <string>${arabic}</string>
      </edit>
    </match>

    <match target="pattern">
      <test name="lang" compare="contains">
        <string>ur</string>
      </test>
      <edit name="family" mode="append" binding="strong">
        <string>${urdu}</string>
      </edit>
    </match>

    <match target="pattern">
      <edit name="family" mode="append" binding="strong">
        <string>${arabic}</string>
      </edit>
    </match>

    <alias>
      <family>system-ui</family>
      <prefer><family>${font}</family></prefer>
    </alias>
    <alias>
      <family>ui-monospace</family>
      <default><family>monospace</family></default>
    </alias>
    <alias>
      <family>-apple-system</family>
      <prefer><family>${font}</family></prefer>
    </alias>
    <alias>
      <family>BlinkMacSystemFont</family>
      <prefer><family>${font}</family></prefer>
    </alias>

    <alias>
      <family>${font}</family>
      <accept>
        <family>${monoFont}</family>
        <family>${emoji}</family>
      </accept>
    </alias>
    <alias>
      <family>sans-serif</family>
      <accept>
        <family>${monoFont}</family>
        <family>${emoji}</family>
      </accept>
    </alias>
    <alias>
      <family>serif</family>
      <accept>
        <family>${emoji}</family>
      </accept>
    </alias>
    <alias>
      <family>monospace</family>
      <accept>
        <family>${emoji}</family>
      </accept>
    </alias>
  </fontconfig>
''
