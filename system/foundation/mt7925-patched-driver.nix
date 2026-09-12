{
  lib,
  stdenv,
  fetchFromGitHub,
  kernel,
}:

stdenv.mkDerivation rec {
  pname = "mt76-mt7925-patched";
  version = "1.5.0-6edf23b";

  src = fetchFromGitHub {
    owner = "zbowling";
    repo = "mt7925";
    rev = "6edf23baa3e966feaf818b10a82015e61bcb6cd6";
    hash = "sha256-bqTPnSR2vIJhvdpb7VYv84exoOA7SHTTAxEuMytJjQ4=";
  };

  sourceRoot = "${src.name}/dkms/src";

  # Linux 7.2 flattened ieee80211_mgmt.u.action and renamed EML delay caps.
  postPatch = ''
    substituteInPlace mt76_connac_mac.c \
      --replace-fail 'mgmt->u.action.u.addba_req.action_code' 'mgmt->u.action.action_code' \
      --replace-fail 'mgmt->u.action.u.addba_req.capab' 'mgmt->u.action.addba_req.capab'
    substituteInPlace mt7925/mac.c \
      --replace-fail 'mgmt->u.action.u.addba_req.action_code' 'mgmt->u.action.action_code'
    substituteInPlace mt7925/mcu.c \
      --replace-fail 'IEEE80211_EML_CAP_EMLSR_PADDING_DELAY' 'IEEE80211_EML_CAP_EML_PADDING_DELAY' \
      --replace-fail 'IEEE80211_EML_CAP_EMLSR_TRANSITION_DELAY' 'IEEE80211_EML_CAP_EML_TRANSITION_DELAY'
  '';

  nativeBuildInputs = kernel.moduleBuildDependencies;

  makeFlags = [
    "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "KERNELRELEASE=${kernel.modDirVersion}"
  ];

  installPhase = ''
    runHook preInstall

    install -Dm444 -t "$out/lib/modules/${kernel.modDirVersion}/updates/dkms" \
      mt76.ko \
      mt76-connac-lib.ko \
      mt792x-lib.ko \
      mt76-usb.ko \
      mt792x-usb.ko \
      mt76-sdio.ko \
      mt7921-common.ko \
      mt7921e.ko \
      mt7921s.ko \
      mt7921u.ko \
      mt7925-common.ko \
      mt7925e.ko

    runHook postInstall
  '';

  meta = {
    description = "Patched out-of-tree mt76 modules for MT7921/MT7925 Wi-Fi";
    homepage = "https://github.com/zbowling/mt7925";
    license = with lib.licenses; [
      gpl2Only
      isc
    ];
    platforms = lib.platforms.linux;
  };
}
