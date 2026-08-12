"""Example: use most portal API features from Python."""

from neo2buddy_wrapper import Neo2BuddyClient, STOCK_APPLET_IDS

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

        catalog = buddy.list_stock_applets()
        print("App Store apps:", catalog.get("bundled_count"))
        for app in catalog.get("applets") or []:
            print(
                f"  {app['slug']:12} "
                f"v{app['version_major']}.{app['version_minor']}{app.get('version_rev') or ''}"
            )
        print("Flash Cards id:", hex(STOCK_APPLET_IDS["flash-cards"]))
        print("Flash decks:", buddy.list_flash_decks())

        # Uncomment when Neo is connected:
        # print("Neo files:", len(buddy.list_neo_files()))
        # buddy.backup_now()
        # buddy.install_stock_applet("flash-cards")
        # buddy.push_flash_deck("en-nl-basic")


if __name__ == "__main__":
    main()
