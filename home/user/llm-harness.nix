{
  config,
  lib,
  pkgs,
  ...
}:
let
  configDir = ../config;
  sourceFile = source: {
    inherit source;
    force = true;
  };
  sourceDir = source: {
    inherit source;
    recursive = true;
    force = true;
  };

  agentRuleFiles = [
    ".agents/AGENTS.md"
    ".pi/agent/AGENTS.md"
  ];

  guideRoots = [
    ".agents/guides"
  ];

  agentSkillRoot = ".agents/skills";
  hermesSkillRoot = ".hermes/skills/personal";

  publicLlmRoot = "${configDir}/llm";
  privateLlm = config.privateConfig.llm;
  llmRoots = [
    privateLlm.root
    publicLlmRoot
  ];
  resolveLlm =
    rel:
    let
      found = lib.findFirst (root: builtins.pathExists "${root}/${rel}") null llmRoots;
    in
    if found == null then throw "LLM path not found: ${rel}" else "${found}/${rel}";
  resolveSkillDir =
    name:
    let
      found = lib.findFirst (root: builtins.pathExists "${root}/skills/${name}/SKILL.md") null llmRoots;
    in
    if found == null then throw "LLM skill not found: ${name}" else "${found}/skills/${name}";

  publicCatalog = builtins.fromJSON (builtins.readFile "${publicLlmRoot}/skill-catalog.json");
  fullSkillCatalog = publicCatalog ++ privateLlm.catalog;
  skillCapabilities = lib.zipAttrsWith (_: lists: lib.unique (lib.concatLists lists)) [
    (import ./skill-capabilities.nix)
    privateLlm.capabilities
  ];
  skillEnabled =
    skill:
    builtins.all (
      capability:
      !(builtins.elem skill.name skillCapabilities.${capability})
      || config.dotfiles.capabilities.${capability}
    ) (builtins.attrNames skillCapabilities);
  skillCatalog = builtins.filter skillEnabled fullSkillCatalog;
  processSkillNames = map (skill: skill.name) (
    builtins.filter (skill: skill.kind == "process") skillCatalog
  );
  sharedSkillNames = map (skill: skill.name) (
    builtins.filter (skill: skill.kind == "shared") skillCatalog
  );
  hermesSharedSkillNames = map (skill: skill.name) (
    builtins.filter (skill: skill.hermes && config.dotfiles.capabilities.hermes) skillCatalog
  );

  # The upstream Superpowers installation owns ~/.agents/skills/superpowers as
  # a namespace of skills. Do not replace that directory with the local meta
  # skill when deploying top-level personal skills.
  processSkillNamesForDeploy = lib.remove "superpowers" processSkillNames;

  sharedSkillReferences = lib.listToAttrs (
    map (skill: {
      inherit (skill) name;
      value = lib.mapAttrs (_: resolveLlm) skill.references;
    }) (builtins.filter (skill: skill.kind == "shared") skillCatalog)
  );

  # Private rules retain domain guidance, but shared approval scope has one owner.
  # Refuse unknown boundaries instead of silently dropping private instructions.
  sharedAgentRules = builtins.readFile "${publicLlmRoot}/AGENTS.md";
  approvalHeading = "### Approval scope";
  approvalParts = lib.splitString "\n\n${approvalHeading}\n\n" sharedAgentRules;
  approvalTail = lib.splitString "\n\n## Verification" (builtins.elemAt approvalParts 1);
  sharedApproval =
    if builtins.length approvalParts != 2 || builtins.length approvalTail != 2 then
      throw "Canonical AGENTS.md must contain one Approval scope block before Verification"
    else
      "${approvalHeading}\n\n${builtins.head approvalTail}";
  selectedAgentRules = builtins.readFile (resolveLlm "AGENTS.md");
  legacyApproval = builtins.filter (lib.hasPrefix "For new behaviour") (
    lib.splitString "\n\n" selectedAgentRules
  );
  effectiveAgentRules =
    if selectedAgentRules == sharedAgentRules then
      sharedAgentRules
    else if
      builtins.length legacyApproval != 1
      || lib.hasInfix approvalHeading selectedAgentRules
      || lib.hasInfix "An approved objective authorizes" selectedAgentRules
    then
      throw "Private AGENTS.md approval boundary changed; reconcile it with canonical Approval scope"
    else
      lib.replaceStrings [ (builtins.head legacyApproval) ] [ sharedApproval ] selectedAgentRules;
  mkAgentRuleFiles = lib.genAttrs agentRuleFiles (_: {
    text = effectiveAgentRules;
    force = true;
  });
  mergedGuides = pkgs.runCommand "agent-guides" { } ''
    mkdir -p "$out"
    cp -a ${publicLlmRoot}/guides/. "$out/"
    chmod -R u+w "$out"
    cp -a ${privateLlm.root}/guides/. "$out/"
  '';
  mkGuideFiles = lib.genAttrs guideRoots (_: sourceDir mergedGuides);
  mkSkillDirectory =
    name: files:
    pkgs.runCommand name { } (
      ''
        mkdir -p "$out"
      ''
      + lib.concatMapStringsSep "\n" (
        file:
        let
          parent = builtins.dirOf file.name;
        in
        ''
          mkdir -p "$out/${parent}"
          cp -L --preserve=mode ${lib.escapeShellArg (toString file.source)} "$out/${file.name}"
        ''
      ) files
    );

  mkProcessSkillFiles = lib.listToAttrs (
    map (skill: {
      name = "${agentSkillRoot}/${skill}";
      value = sourceFile (
        mkSkillDirectory "process-skill-${skill}" [
          {
            name = "SKILL.md";
            source = resolveLlm "process-skills/${skill}/SKILL.md";
          }
        ]
      );
    }) processSkillNamesForDeploy
  );

  superpowersMetaSkillFile = {
    "${agentSkillRoot}/superpowers/SKILL.md" =
      sourceFile "${configDir}/llm/process-skills/superpowers/SKILL.md";
  };

  mkReferenceLibraryFiles = root: {
    "${root}/references" = sourceDir "${configDir}/llm/references";
  };

  localSkillReferences =
    skillDir:
    let
      referencesDir = "${skillDir}/references";
      entries = if builtins.pathExists referencesDir then builtins.readDir referencesDir else { };
    in
    lib.filterAttrs (name: type: type == "regular" && lib.hasSuffix ".md" name) entries;

  localSkillScripts =
    skillDir:
    let
      scriptsDir = "${skillDir}/scripts";
      entries = if builtins.pathExists scriptsDir then builtins.readDir scriptsDir else { };
    in
    lib.filterAttrs (_: type: type == "regular") entries;

  sharedSkillFiles =
    skill:
    let
      skillDir = resolveSkillDir skill;
    in
    [
      {
        name = "SKILL.md";
        source = "${skillDir}/SKILL.md";
      }
    ]
    ++ lib.optional (builtins.pathExists "${skillDir}/agents/openai.yaml") {
      name = "agents/openai.yaml";
      source = "${skillDir}/agents/openai.yaml";
    }
    ++ lib.mapAttrsToList (referenceName: _: {
      name = "references/${referenceName}";
      source = "${skillDir}/references/${referenceName}";
    }) (localSkillReferences skillDir)
    ++ lib.mapAttrsToList (scriptName: _: {
      name = "scripts/${scriptName}";
      source = "${skillDir}/scripts/${scriptName}";
    }) (localSkillScripts skillDir)
    ++ lib.mapAttrsToList (referenceName: referenceSource: {
      name = "references/${referenceName}";
      source = referenceSource;
    }) sharedSkillReferences.${skill};

  # Directory aliases must resolve to the same store path so Pi can dedupe
  # symlink skills instead of reporting a name collision.
  skillDirectoryAliases = lib.filterAttrs (_: target: builtins.elem target sharedSkillNames) {
    microsoft-login = "outlook-login";
  };
  deployedSharedSkillNames = builtins.filter (
    skill: !builtins.hasAttr skill skillDirectoryAliases
  ) sharedSkillNames;
  mkSharedSkillSources = lib.genAttrs deployedSharedSkillNames (
    skill: mkSkillDirectory "shared-skill-${skill}" (sharedSkillFiles skill)
  );
  mkSharedSkillFiles = lib.listToAttrs (
    (map (skill: {
      name = "${agentSkillRoot}/${skill}";
      value = sourceFile mkSharedSkillSources.${skill};
    }) deployedSharedSkillNames)
    ++ lib.mapAttrsToList (alias: target: {
      name = "${agentSkillRoot}/${alias}";
      value = sourceFile mkSharedSkillSources.${target};
    }) skillDirectoryAliases
  );

  mkHermesSharedSkillFiles = lib.listToAttrs (
    map (skill: {
      name = "${hermesSkillRoot}/${skill}";
      value = sourceFile (mkSkillDirectory "hermes-shared-skill-${skill}" (sharedSkillFiles skill));
    }) hermesSharedSkillNames
  );
in
{
  options.dotfiles.publicModuleRoot = lib.mkOption {
    type = lib.types.path;
    readOnly = true;
    default = ./.;
    description = "Reusable public modules consumed by the private overlay.";
  };

  options.dotfiles.llmScriptRoot = lib.mkOption {
    type = lib.types.path;
    readOnly = true;
    default = ../config/llm/scripts;
    description = "Public LLM Python scripts consumed by private overlay modules.";
  };

  config.home.file =
    mkAgentRuleFiles
    // mkGuideFiles
    // mkSharedSkillFiles
    // mkHermesSharedSkillFiles
    // mkProcessSkillFiles
    // superpowersMetaSkillFile
    // mkReferenceLibraryFiles ".agents";

  config.home.activation.migrateAgentSkillDirectories =
    lib.hm.dag.entryBetween [ "linkGeneration" ] [ "writeBoundary" ]
      ''
        for skill in ${
          lib.concatMapStringsSep " " lib.escapeShellArg (sharedSkillNames ++ processSkillNamesForDeploy)
        }; do
          skillPath="$HOME/${agentSkillRoot}/$skill"
          if [ ! -d "$skillPath" ] || [ -L "$skillPath" ]; then
            continue
          fi

          unexpectedFile="$(${pkgs.findutils}/bin/find "$skillPath" ! -type d ! -type l -print -quit)"
          if [ -n "$unexpectedFile" ]; then
            echo "Refusing to replace skill directory containing an unmanaged file: $unexpectedFile" >&2
            exit 1
          fi

          unsafeLink=""
          while IFS= read -r -d "" linkPath; do
            case "$(${pkgs.coreutils}/bin/readlink "$linkPath")" in
              /nix/store/*-home-manager-files/${agentSkillRoot}/"$skill"/*) ;;
              *)
                unsafeLink="$linkPath"
                break
                ;;
            esac
          done < <(${pkgs.findutils}/bin/find "$skillPath" -type l -print0)

          if [ -n "$unsafeLink" ]; then
            echo "Refusing to replace skill directory containing an unmanaged link: $unsafeLink" >&2
            exit 1
          fi

          ${pkgs.coreutils}/bin/rm -r -- "$skillPath"
        done
      '';

}
