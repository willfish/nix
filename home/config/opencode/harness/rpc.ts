import { Rpc } from "@opencode/plugin/rpc";

const state = {
  type: "object",
  properties: {
    sessionID: { type: "string" },
    text: { type: "string" },
    goal: { type: ["object", "null"], additionalProperties: true },
  },
  required: ["sessionID", "text", "goal"],
  additionalProperties: false,
};

export const Workflow = Rpc.define({
  id: "dotfiles.workflow",
  methods: {
    goal: {
      input: {
        type: "object",
        properties: {
          sessionID: { type: "string" },
          command: { type: "string" },
          replace: { type: "boolean" },
        },
        required: ["sessionID", "command"],
        additionalProperties: false,
      },
      output: state,
    },
  },
  events: { goal: { schema: state } },
});
