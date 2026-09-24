# These pure theme imports are also consumed outside the flake module graph.
# Resolve the flake's locked, data-only inputs instead of maintaining another pin.
let
  lock = builtins.fromJSON (builtins.readFile ../../../flake.lock);
  rootInputs = lock.nodes.${lock.root}.inputs;
  names = builtins.filter (
    name: name == "omarchy" || builtins.match "omarchy-theme-.+" name != null
  ) (builtins.attrNames rootInputs);
  resolve =
    name:
    let
      reference = rootInputs.${name};
      node = lock.nodes.${reference};
    in
    assert builtins.isString reference;
    assert (node.flake or true) == false;
    builtins.fetchTree node.locked;
in
builtins.listToAttrs (
  map (name: {
    inherit name;
    value = resolve name;
  }) names
)
