package main

import "testing"

// With no desktop map initialised, reaching application lookup would panic.
// Reject every nonempty suffix before either lookup or process execution.
func TestLauncherRejectsQueryArguments(t *testing.T) {
	for _, action := range []string{ActionStart, ActionNewInstance} {
		for _, args := range []string{"https://example.org", "; touch /tmp/launcher-unsafe", "$(id)", " ", "'quoted'"} {
			t.Run(action+args, func(t *testing.T) {
				Activate(false, "missing.desktop", action, "query", args, 0, nil)
			})
		}
	}
}
