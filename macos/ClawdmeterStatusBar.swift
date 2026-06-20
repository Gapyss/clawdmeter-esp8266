import AppKit
import Foundation

private let label = "com.user.clawdmeter"
private let repoRoot = "__CLAWDMETER_REPO_ROOT__"
private let deviceURL = "http://clawdmeter.local"

@main
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let menu = NSMenu()
    private let stateItem = NSMenuItem(title: "Checking...", action: nil, keyEquivalent: "")
    private let startItem = NSMenuItem(title: "Start Daemon", action: #selector(startDaemon), keyEquivalent: "s")
    private let stopItem = NSMenuItem(title: "Stop Daemon", action: #selector(stopDaemon), keyEquivalent: "x")
    private let restartItem = NSMenuItem(title: "Restart Daemon", action: #selector(restartDaemon), keyEquivalent: "r")
    private var timer: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)

        statusItem.button?.title = "Clawdmeter"
        statusItem.button?.toolTip = "Clawdmeter daemon"
        statusItem.menu = menu

        stateItem.isEnabled = false
        menu.addItem(stateItem)
        menu.addItem(NSMenuItem.separator())
        menu.addItem(startItem)
        menu.addItem(stopItem)
        menu.addItem(restartItem)
        menu.addItem(NSMenuItem.separator())
        menu.addItem(NSMenuItem(title: "Open Dashboard", action: #selector(openDashboard), keyEquivalent: "o"))
        menu.addItem(NSMenuItem(title: "Open Log Folder", action: #selector(openLogFolder), keyEquivalent: "l"))
        menu.addItem(NSMenuItem.separator())
        menu.addItem(NSMenuItem(title: "Quit", action: #selector(quit), keyEquivalent: "q"))

        menu.items.forEach { $0.target = self }
        stateItem.target = nil

        refreshStatus()
        timer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
            self?.refreshStatus()
        }
    }

    @objc private func startDaemon() {
        do {
            try installLaunchAgent()
            _ = run("/bin/launchctl", ["enable", serviceTarget()])
            let boot = run("/bin/launchctl", ["bootstrap", guiTarget(), launchAgentPath()])
            if boot.exitCode != 0 && !boot.stderr.contains("Bootstrap failed: 5") {
                showError("Could not start launch agent", boot.stderr)
                refreshStatus()
                return
            }
            let kick = run("/bin/launchctl", ["kickstart", "-k", serviceTarget()])
            if kick.exitCode != 0 {
                showError("Could not kickstart daemon", kick.stderr)
            }
        } catch {
            showError("Could not install launch agent", error.localizedDescription)
        }
        refreshStatus()
    }

    @objc private func stopDaemon() {
        let service = run("/bin/launchctl", ["bootout", serviceTarget()])
        if service.exitCode != 0 {
            _ = run("/bin/launchctl", ["bootout", guiTarget(), launchAgentPath()])
        }
        refreshStatus()
    }

    @objc private func restartDaemon() {
        stopDaemon()
        startDaemon()
    }

    @objc private func openDashboard() {
        if let url = URL(string: deviceURL) {
            NSWorkspace.shared.open(url)
        }
    }

    @objc private func openLogFolder() {
        NSWorkspace.shared.open(URL(fileURLWithPath: "\(repoRoot)/daemon", isDirectory: true))
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }

    private func refreshStatus() {
        let result = run("/bin/launchctl", ["print", serviceTarget()])
        let loaded = result.exitCode == 0
        let pid = parsePid(result.stdout)

        if let pid {
            statusItem.button?.title = "Clawdmeter On"
            stateItem.title = "Running (PID \(pid))"
        } else if loaded {
            statusItem.button?.title = "Clawdmeter On"
            stateItem.title = "Loaded"
        } else {
            statusItem.button?.title = "Clawdmeter Off"
            stateItem.title = "Stopped"
        }

        startItem.isEnabled = !loaded
        stopItem.isEnabled = loaded
        restartItem.isEnabled = loaded
    }

    private func installLaunchAgent() throws {
        let manager = FileManager.default
        let launchAgents = manager.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/LaunchAgents", isDirectory: true)
        try manager.createDirectory(at: launchAgents, withIntermediateDirectories: true)

        let plist = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
          "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
          <key>Label</key>
          <string>\(label)</string>
          <key>ProgramArguments</key>
          <array>
            <string>/usr/bin/python3</string>
            <string>\(xmlEscape("\(repoRoot)/daemon/claudemeter_daemon.py"))</string>
          </array>
          <key>RunAtLoad</key>
          <true/>
          <key>KeepAlive</key>
          <true/>
          <key>StandardOutPath</key>
          <string>\(xmlEscape("\(repoRoot)/daemon/clawdmeter.log"))</string>
          <key>StandardErrorPath</key>
          <string>\(xmlEscape("\(repoRoot)/daemon/clawdmeter.err.log"))</string>
        </dict>
        </plist>
        """

        try plist.write(toFile: launchAgentPath(), atomically: true, encoding: .utf8)
    }

    private func parsePid(_ output: String) -> String? {
        for line in output.split(separator: "\n") {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("pid = ") {
                return String(trimmed.dropFirst("pid = ".count))
            }
        }
        return nil
    }

    private func launchAgentPath() -> String {
        "\(FileManager.default.homeDirectoryForCurrentUser.path)/Library/LaunchAgents/\(label).plist"
    }

    private func guiTarget() -> String {
        "gui/\(getuid())"
    }

    private func serviceTarget() -> String {
        "\(guiTarget())/\(label)"
    }

    private func run(_ executable: String, _ arguments: [String]) -> CommandResult {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments

        let stdout = Pipe()
        let stderr = Pipe()
        process.standardOutput = stdout
        process.standardError = stderr

        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return CommandResult(exitCode: 127, stdout: "", stderr: error.localizedDescription)
        }

        return CommandResult(
            exitCode: process.terminationStatus,
            stdout: String(data: stdout.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? "",
            stderr: String(data: stderr.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        )
    }

    private func showError(_ message: String, _ detail: String) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = message
        alert.informativeText = detail.isEmpty ? "No details were returned." : detail
        alert.runModal()
    }

    private func xmlEscape(_ value: String) -> String {
        value
            .replacingOccurrences(of: "&", with: "&amp;")
            .replacingOccurrences(of: "\"", with: "&quot;")
            .replacingOccurrences(of: "'", with: "&apos;")
            .replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
    }
}

private struct CommandResult {
    let exitCode: Int32
    let stdout: String
    let stderr: String
}
