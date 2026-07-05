#!/usr/bin/env bash
set -euo pipefail

# allowlist_firewall.sh
# Usage: sudo ./allowlist_firewall.sh 1.2.3.4 [5.6.7.8 ...]
# The script will configure UFW if present, otherwise fall back to iptables.
# It restricts incoming connections to a small set of ports (default: 22,8001,8554)
# and a UDP ephemeral range (default: 49152:65535) for WebRTC media.
# allowing only the provided source IP addresses. Use with caution when remote.

PORTS=(22 8001 8554)
UDP_PORTS="49152:65535"
METADATA_FILE=/etc/allowlist_firewall.meta

# Mode detection: --revert / --disable to undo changes, otherwise treat args as IPs
if [ "${1:-}" = "--revert" ] || [ "${1:-}" = "--disable" ]; then
  MODE=revert
  shift || true
else
  MODE=apply
fi

IFS=' ' read -r -a IPS <<< "${@}"

if [ "$MODE" = apply ] && [ ${#IPS[@]} -eq 0 ]; then
  echo "Usage: sudo $0 <allowed-ip-1> [allowed-ip-2 ...]"
  echo "Example: sudo $0 203.0.113.45"
  echo "To revert: sudo $0 --revert" 
  exit 1
fi

echo "Allowed IPs: ${IPS[*]}"
echo "TCP Ports: ${PORTS[*]}"
echo "UDP Range: ${UDP_PORTS}"

# Safety: if running remotely, warn user about lockout
if [ -n "${SSH_CONNECTION:-}" ]; then
  echo "WARNING: You appear to be running this over SSH. If you are not including your current public IP in the allowed list, you may lock yourself out." >&2
fi

command_exists() { command -v "$1" >/dev/null 2>&1; }

if command_exists ufw; then
  echo "Configuring UFW..."
  # Reset to known state
  sudo ufw --force reset
  sudo ufw default deny incoming
  sudo ufw default allow outgoing

  # Allow local loopback always
  sudo ufw allow from 127.0.0.1 to any

  for ip in "${IPS[@]}"; do
    for p in "${PORTS[@]}"; do
      echo "Allowing $ip -> port $p/tcp"
      sudo ufw allow from "$ip" to any port "$p" proto tcp
    done
    echo "Allowing $ip -> ports ${UDP_PORTS}/udp (WebRTC ICE/media)"
    sudo ufw allow from "$ip" to any port "${UDP_PORTS}" proto udp
  done

  echo "Enabling UFW..."
  sudo ufw --force enable
  sudo ufw status numbered
  # Write metadata so we can revert later
  echo "method=ufw" | sudo tee "$METADATA_FILE" >/dev/null || true
  echo "UFW configured."
  exit 0
fi

# Fallback: iptables
if command_exists iptables; then
  echo "UFW not found, using iptables."

  # Save current rules (non-destructive backup)
  TIMESTAMP=$(date +%s)
  BACKUP=/etc/iptables-backup-${TIMESTAMP}.rules
  sudo iptables-save > "$BACKUP" || true
  echo "Saved existing iptables rules to $BACKUP"

  # Do NOT flush filter chains — preserves Docker's DOCKER/DOCKER-USER/FORWARD chains
  # Do NOT change FORWARD policy — Docker needs it for container networking

  # Default deny incoming on host, allow outgoing
  sudo iptables -P INPUT DROP
  sudo iptables -P OUTPUT ACCEPT

  # Insert allow rules at top of INPUT (so they precede any existing Docker rules)
  sudo iptables -I INPUT -i lo -j ACCEPT
  sudo iptables -I INPUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT

  for ip in "${IPS[@]}"; do
    for p in "${PORTS[@]}"; do
      echo "Allowing $ip -> port $p/tcp"
      sudo iptables -I INPUT -p tcp -s "$ip" --dport "$p" -j ACCEPT
    done
    echo "Allowing $ip -> ports ${UDP_PORTS}/udp (WebRTC ICE/media)"
    sudo iptables -I INPUT -p udp -s "$ip" --dport "${UDP_PORTS}" -j ACCEPT
  done

  # Optional: allow ICMP (ping)
  sudo iptables -I INPUT -p icmp -j ACCEPT

  # Persist rules if iptables-persistent is available
  if command_exists netfilter-persistent || command_exists iptables-save; then
    if command_exists netfilter-persistent; then
      sudo netfilter-persistent save || true
      echo "Saved rules via netfilter-persistent"
    else
      sudo iptables-save | sudo tee /etc/iptables.rules >/dev/null || true
      echo "Saved rules to /etc/iptables.rules (manual persistence may be required)"
    fi
  fi

  # Write metadata so we can revert later
  echo "method=iptables" | sudo tee "$METADATA_FILE" >/dev/null || true
  echo "backup=$BACKUP" | sudo tee -a "$METADATA_FILE" >/dev/null || true

  echo "Iptables configured. Backup at $BACKUP"
  exit 0
fi

echo "Neither 'ufw' nor 'iptables' found on system. Please install one of them and re-run this script."
exit 2

# Revert logic: restore previous rules and disable firewall
revert() {
  echo "Attempting revert/disable..."
  if [ -f "$METADATA_FILE" ]; then
    . "$METADATA_FILE"
    if [ "${method:-}" = "ufw" ]; then
      echo "Disabling UFW and resetting to defaults..."
      sudo ufw --force disable || true
      sudo ufw --force reset || true
      echo "UFW disabled."
      sudo rm -f "$METADATA_FILE" || true
      exit 0
    elif [ "${method:-}" = "iptables" ]; then
      echo "Restoring iptables from backup: ${backup:-}" 
      if [ -n "${backup:-}" ] && [ -f "${backup}" ]; then
        sudo iptables-restore < "$backup" || true
        echo "Restored iptables from $backup"
      else
        # Try common fallback
        if [ -f /etc/iptables.rules ]; then
          sudo iptables-restore < /etc/iptables.rules || true
          echo "Restored iptables from /etc/iptables.rules"
        else
          echo "No iptables backup found to restore." >&2
        fi
      fi
      sudo rm -f "$METADATA_FILE" || true
      exit 0
    fi
  fi

  # Generic fallbacks if metadata missing
  if command_exists ufw; then
    echo "No metadata found; disabling UFW (fallback)."
    sudo ufw --force disable || true
    sudo ufw --force reset || true
    exit 0
  fi

  if command_exists iptables; then
    echo "No metadata found; attempting to restore /etc/iptables.rules if present."
    if [ -f /etc/iptables.rules ]; then
      sudo iptables-restore < /etc/iptables.rules || true
      echo "Restored /etc/iptables.rules"
      exit 0
    fi
  fi

  echo "Nothing to revert or unknown state."
  exit 1
}

if [ "$MODE" = revert ]; then
  revert
fi
