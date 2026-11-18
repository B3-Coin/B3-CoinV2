// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestFinalAvailabilityBudgetIncludesDiagnostics(t *testing.T) {
	value := receiptResult()
	value.Passed = true
	value.Header = "a"
	base, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	value.Header = strings.Repeat("a", maxWitnessBytes-len(base))
	raw, err := encodeAvailabilityJSON(value)
	if err != nil || len(raw) != maxWitnessBytes || raw[len(raw)-1] != '\n' {
		t.Fatal("exact final wire budget refused", err)
	}
	value.FailureStages = map[string]uint64{"receipt_availability": 1}
	if raw, err := encodeAvailabilityJSON(value); err == nil || len(raw) != 0 {
		t.Fatal("diagnostics overflow emitted partial receipt output")
	}
}
