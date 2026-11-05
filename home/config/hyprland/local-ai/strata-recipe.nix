{
  id = "qwen3.8-flash-next.strata.256k.rtx-5090-32gb";
  name = "Qwen3.8-Flash-Next (Unsloth v3.1)";
  family = "qwen";
  format = "GGUF · UD-IQ4_XS-v31";
  engine = "strata";
  servedName = "qwen3.8-flash-next-unsloth-ud-iq4_xs-v31";
  sizeGb = 93.7;
  cards = 1;
  image = "ghcr.io/niko1221/strata@sha256:871e1e79c063b240184b08075ba0676b6cd2585eac11f76a9d179f4739d2c857";
  minDriver = "580.65";
  weights = [
    {
      repository = "unsloth/Qwen3.8-Flash-Next-GGUF";
      revision = "d96201194f103c906994f283f504209dc8d2a10c";
      sizeGb = 93.7;
      layout = "dir";
      mountPath = "/models";
      dir = "v31-iq4";
      files = builtins.concatStringsSep "," (
        map (part: "Qwen3.8-Flash-Next-UD-IQ4_XS-0000${part}-of-00004.gguf") [
          "1"
          "2"
          "3"
          "4"
        ]
      );
    }
  ];
  asset = {
    name = "strata-server.sh";
    mountPath = "/run/strata-server.sh";
    text = builtins.readFile ./strata-server.sh;
  };
  prepare = {
    at = "/prepared";
    sizeGb = 6;
    gpu = true;
    args = [
      "-eu"
      "-c"
      ''
        export STRATA_CONTROL_INSTALL=1 STRATA_INSTALL_DIR=/prepared/install
        mkdir -p "$STRATA_INSTALL_DIR/third_party"
        ln -s /opt/strata/seed/third_party/llama.cpp "$STRATA_INSTALL_DIR/third_party/llama.cpp"
        cd /opt/strata/terminal
        exec python/bin/python3 setup.py --yes --family unsloth --model UD-IQ4_XS-v31 \
          --gguf-dir /models/v31-iq4/UD-IQ4_XS --data-dir /prepared --context 262144 \
          --vision no --mtp off --parallel 1 --vram-reserve-mib 4096 \
          --host 0.0.0.0 --port 8080 --no-browser --no-start
      ''
    ];
    verifyArgs = [
      "-eu"
      "-c"
      ''
        test -s /prepared/install/strata-unsloth-ud-iq4_xs-v31.json
        test -s /prepared/packs/unsloth-ud-iq4_xs-v31/native_experts.txt
        test -s /prepared/packs/unsloth-ud-iq4_xs-v31/dense.bin
        test -s /prepared/packs/unsloth-ud-iq4_xs-v31/tokenizer/vocab.json
      ''
    ];
  };
  scratch = null;
  launch = {
    entrypoint = "/bin/sh";
    arguments = [ "/run/strata-server.sh" ];
    environment = {
      CUDA_DEVICE_ORDER = "PCI_BUS_ID";
      STRATA_ALLOWED_HOSTS = "engine";
      STRATA_ROUTE_TAIL_SKIP = "0";
      XDG_CONFIG_HOME = "/tmp/strata-config";
      XDG_STATE_HOME = "/tmp/strata-state";
    };
    port = 8080;
    shm = "2g";
    resources.memlockUnlimited = true;
  };
  serving = {
    ctxTokens = 262144;
    kvTokens = 262144;
  };
  capabilities = {
    chat = true;
    reasoning = true;
    tools = true;
    vision = false;
  };
  needs = {
    host_ram_gb = 96;
    disk_gb = 105;
    fast_storage = "nvme";
  };
}
