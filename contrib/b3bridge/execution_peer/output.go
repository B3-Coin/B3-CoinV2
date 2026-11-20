// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"encoding/json"
	"errors"
)

// Check the final wire result after collectors add counters and diagnostics.
// Checking only the inner peer response leaves an unbounded metadata tail.
func encodeAvailabilityJSON(value any) ([]byte, error) {
	raw, err := json.Marshal(value)
	if err != nil || len(raw) >= maxWitnessBytes {
		return nil, errors.New("final availability output byte bound exceeded")
	}
	return append(raw, '\n'), nil
}
