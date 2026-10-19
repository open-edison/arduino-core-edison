package main

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"

	"golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/knownhosts"
)

type client struct{ *ssh.Client }

func (c *client) run(cmd string, stdin io.Reader) error {
	s, err := c.NewSession()
	if err != nil {
		return err
	}
	defer s.Close()
	var stderr bytes.Buffer
	s.Stdin, s.Stderr = stdin, &stderr
	if err := s.Run(cmd); err != nil {
		if msg := strings.TrimSpace(stderr.String()); msg != "" {
			return fmt.Errorf("%w: %s", err, msg)
		}
		return err
	}
	return nil
}

func (c *client) output(cmd string) (string, error) {
	s, err := c.NewSession()
	if err != nil {
		return "", err
	}
	defer s.Close()
	out, err := s.Output(cmd)
	return string(out), err
}

// loadKey loads this computer's upload key, creating it on first use.
func loadKey(dir string) (ssh.Signer, error) {
	path := filepath.Join(dir, "id_ed25519")
	if data, err := os.ReadFile(path); err == nil {
		return ssh.ParsePrivateKey(data)
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return nil, err
	}
	host, _ := os.Hostname()
	block, err := ssh.MarshalPrivateKey(priv, "edison-upload@"+host)
	if err != nil {
		return nil, err
	}
	data := pem.EncodeToMemory(block)
	if err := os.WriteFile(path, data, 0o600); err != nil {
		return nil, err
	}
	debugf("Created upload key %s", path)
	return ssh.ParsePrivateKey(data)
}

// hostKeyCallback checks the board's host key. On the USB link every board has the same
// address and the cable is the board, so any key is accepted. On the network, the key is
// remembered the first time and must match afterwards.
func hostKeyCallback(dir string, usb bool) (ssh.HostKeyCallback, error) {
	if usb {
		return func(_ string, _ net.Addr, key ssh.PublicKey) error {
			debugf("Host key %s (USB link, not checked)", ssh.FingerprintSHA256(key))
			return nil
		}, nil
	}
	path := filepath.Join(dir, "known_hosts")
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDONLY, 0o600)
	if err != nil {
		return nil, err
	}
	f.Close()
	check, err := knownhosts.New(path)
	if err != nil {
		return nil, err
	}
	// Keys remembered by this callback: the file is only read once, and connect may dial
	// twice (key, then password).
	added := map[string]bool{}
	return func(host string, remote net.Addr, key ssh.PublicKey) error {
		id := knownhosts.Normalize(host) + " " + string(key.Marshal())
		if added[id] {
			return nil
		}
		err := check(host, remote, key)
		var keyErr *knownhosts.KeyError
		if errors.As(err, &keyErr) && len(keyErr.Want) == 0 {
			logf("New board at %s; remembering its host key %s", host, ssh.FingerprintSHA256(key))
			added[id] = true
			return appendKnownHost(path, host, key)
		}
		if errors.As(err, &keyErr) {
			return fmt.Errorf("the board at %s has a different host key than last time.\n"+
				"If it was reflashed, or is another board, run\n"+
				"  edison-upload -forget %s\nand upload again. Otherwise something is "+
				"impersonating it", host, knownhosts.Normalize(host))
		}
		return err
	}, nil
}

func appendKnownHost(path, host string, key ssh.PublicKey) error {
	f, err := os.OpenFile(path, os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	defer f.Close()
	_, err = fmt.Fprintln(f, knownhosts.Line([]string{knownhosts.Normalize(host)}, key))
	return err
}

func forgetHost(path, host string) error {
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	} else if err != nil {
		return err
	}
	want := knownhosts.Normalize(host)
	var keep []string
	for _, line := range strings.Split(strings.TrimRight(string(data), "\n"), "\n") {
		if f := strings.Fields(line); len(f) > 0 && f[0] == want {
			continue
		}
		keep = append(keep, line)
	}
	out := strings.Join(keep, "\n")
	if out != "" {
		out += "\n"
	}
	return os.WriteFile(path, []byte(out), 0o600)
}

// dial logs in to addr. The host key is checked against addr as given, even when the name
// is resolved some other way (dialAddr).
func dial(addr, user string, hostKey ssh.HostKeyCallback, auth ...ssh.AuthMethod) (*client, error) {
	conn, err := net.DialTimeout("tcp", dialAddr(addr), 10*time.Second)
	if err != nil {
		return nil, err
	}
	sc, chans, reqs, err := ssh.NewClientConn(conn, addr, &ssh.ClientConfig{
		User:            user,
		Auth:            auth,
		HostKeyCallback: hostKey,
	})
	if err != nil {
		conn.Close()
		return nil, err
	}
	return &client{ssh.NewClient(sc, chans, reqs)}, nil
}

func isAuthError(err error) bool {
	return err != nil && strings.Contains(err.Error(), "unable to authenticate")
}

// connect logs in with the upload key; if the board doesn't know it yet, it logs in with
// the password and installs the key.
func connect(dir, addr string, usb bool, user, password string) (*client, error) {
	key, err := loadKey(dir)
	if err != nil {
		return nil, fmt.Errorf("upload key: %w", err)
	}
	hostKey, err := hostKeyCallback(dir, usb)
	if err != nil {
		return nil, err
	}
	c, err := dial(addr, user, hostKey, ssh.PublicKeys(key))
	if err == nil {
		debugf("Logged in with the upload key")
		return c, nil
	}
	if !isAuthError(err) {
		return nil, unreachable(addr, usb, err)
	}
	if password == "" {
		return nil, errors.New("the board doesn't know this computer yet. Enter the " + user +
			" user's password\nwhen the IDE asks for it (or pass -password); it's only " +
			"needed for the first upload")
	}
	answer := func(_, _ string, questions []string, _ []bool) ([]string, error) {
		answers := make([]string, len(questions))
		for i := range answers {
			answers[i] = password
		}
		return answers, nil
	}
	c, err = dial(addr, user, hostKey, ssh.Password(password), ssh.KeyboardInteractive(answer))
	if isAuthError(err) {
		return nil, fmt.Errorf("wrong password for %s on the board", user)
	} else if err != nil {
		return nil, unreachable(addr, usb, err)
	}
	pub := strings.TrimSpace(string(ssh.MarshalAuthorizedKey(key.PublicKey())))
	comment := "edison-upload"
	if host, _ := os.Hostname(); host != "" {
		comment += "@" + host
	}
	line := pub + " " + comment
	// The key is a plain string of base64 and safe characters; quote it for the shell.
	cmd := fmt.Sprintf("umask 077 && mkdir -p ~/.ssh && "+
		"(grep -qF '%s' ~/.ssh/authorized_keys 2>/dev/null || echo '%s' >> ~/.ssh/authorized_keys)",
		pub, line)
	if err := c.run(cmd, nil); err != nil {
		c.Close()
		return nil, fmt.Errorf("installing the upload key: %w", err)
	}
	logf("Installed this computer's upload key on the board; the password won't be needed again.")
	return c, nil
}

func unreachable(addr string, usb bool, err error) error {
	if usb {
		return fmt.Errorf("can't reach the board at %s over the USB cable (%v).\n"+
			"Is it booted, with J16 connected to this computer?", addr, err)
	}
	return fmt.Errorf("can't reach the board at %s (%v)", addr, err)
}
