//go:build !linux

package main

func handlePlatformCommand(args []string) ([]string, bool, error) {
	return args, false, nil
}

func configureControlSocketOwnership(_ string) error { return nil }
