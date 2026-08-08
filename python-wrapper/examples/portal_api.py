"""Example: use most portal API features from Python."""

from neo2buddy_wrapper import Neo2BuddyClient

HOST = "192.168.4.1"
PASSWORD = "neo2buddy"


def main() -> None:
    with Neo2BuddyClient(HOST, password=PASSWORD) as buddy:
        print("Status:", buddy.status().get("usb_connected"))
        print("Settings:", buddy.get_settings().get("neo_label"))
        print("Wi-Fi:", buddy.get_wifi())
        print("Neo mode:", buddy.neo_mode())
        print("Local backups:", len(buddy.list_backups()))
        print("Sync config provider:", buddy.get_sync_config().get("provider"))

        # Uncomment when Neo is connected:
        # print("Neo files:", len(buddy.list_neo_files()))
        # buddy.backup_now()


if __name__ == "__main__":
    main()
