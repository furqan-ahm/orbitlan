package main

import (
	"reflect"
	"regexp"
	"testing"
)

func TestNormalizeJoinCommand(t *testing.T) {
	got, createRoom, err := normalizeCommandArgs([]string{"join", "ab23xy", "-name", "alice"})
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"-code", "ab23xy", "-name", "alice"}
	if createRoom || !reflect.DeepEqual(got, want) {
		t.Fatalf("got args=%v create=%v, want args=%v create=false", got, createRoom, want)
	}
}

func TestNormalizeNewCommand(t *testing.T) {
	got, createRoom, err := normalizeCommandArgs([]string{"new", "-relay", "off"})
	if err != nil {
		t.Fatal(err)
	}
	if !createRoom || !reflect.DeepEqual(got, []string{"-relay", "off"}) {
		t.Fatalf("got args=%v create=%v", got, createRoom)
	}
}

func TestNormalizeJoinCommandNeedsCode(t *testing.T) {
	if _, _, err := normalizeCommandArgs([]string{"join"}); err == nil {
		t.Fatal("expected missing-code error")
	}
}

func TestRandomRoomCode(t *testing.T) {
	valid := regexp.MustCompile(`^[abcdefghjkmnpqrstuvwxyz2-9]{6}$`)
	first := randomRoomCode()
	if !valid.MatchString(first) {
		t.Fatalf("invalid room code %q", first)
	}
	for i := 0; i < 20; i++ {
		if next := randomRoomCode(); !valid.MatchString(next) {
			t.Fatalf("invalid room code %q", next)
		}
	}
}
