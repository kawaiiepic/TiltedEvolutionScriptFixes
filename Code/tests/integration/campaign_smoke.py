import argparse
from contextlib import ExitStack
import os
import pathlib
import pty
import select
import socket
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description="Exercise the shared campaign over the server's real transport.")
parser.add_argument("server", type=pathlib.Path)
parser.add_argument("client", type=pathlib.Path)
parser.add_argument("build_info", type=pathlib.Path)
args = parser.parse_args()
server = args.server.resolve(strict=True)
harness = args.client.resolve(strict=True)
version = next(
    line.split('"')[1] for line in args.build_info.read_text().splitlines()
    if line.startswith("#define BUILD_PROTOCOL ")
)

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as bound:
    bound.bind(("127.0.0.1", 0))
    port = bound.getsockname()[1]

class Runner:
    def __init__(self, directory):
        self.directory = directory
        self.log = b""
        self.master, slave = pty.openpty()
        env = os.environ.copy()
        env["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, (str(server.parent), env.get("LD_LIBRARY_PATH"))))
        self.proc = subprocess.Popen(
            [server], cwd=directory,
            stdin=slave, stdout=slave, stderr=slave, env=env
        )
        os.close(slave)

    def expect(self, output, timeout=20):
        deadline = time.monotonic() + timeout
        while output.encode() not in self.log and time.monotonic() < deadline:
            if select.select([self.master], [], [], 0.15)[0]:
                try:
                    self.log += os.read(self.master, 65536)
                except OSError:
                    break
        assert output.encode() in self.log, self.log.decode(errors="replace")

    def command(self, line):
        os.write(self.master, (line + "\n").encode())

    def __enter__(self):
        return self

    def __exit__(self, *_):
        try:
            if self.proc.poll() is None:
                self.command("/quit")
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait(timeout=5)
        finally:
            if select.select([self.master], [], [], 0)[0]:
                try:
                    self.log += os.read(self.master, 65536)
                except OSError:
                    pass
            os.close(self.master)
        print(self.log.decode(errors="replace"))

def test_client(mode, build_id=version):
    subprocess.run([harness, mode, build_id, f"127.0.0.1:{port}"], check=True, timeout=15)

with tempfile.TemporaryDirectory(prefix="campaign-smoke-") as tmp:
    directory = pathlib.Path(tmp)
    config = directory / "config"
    config.mkdir()
    (config / "STServer.ini").write_text(
        f"[GameServer]\nuPort={port}\n[Gameplay]\nbAutoPartyJoin=false\nuPlayerRespawnDelay=1\n"
    )
    (directory / "shared-quest-policies.conf").write_text(
        'personal light "Quest Pack.esl" 0x8AC\n'
    )
    (directory / "shared-npc-life-policies.conf").write_text(
        'personal light "Quest Pack.esl" 0x89B\n'
    )
    resource = directory / "resources" / "smoke-check"
    resource.mkdir(parents=True)
    (resource / "smoke-check.manifest").write_text(
        '[Resource]\nname=smoke-check\nversion=1.0.0\napiset=1.0.0\n'
        'description=Inspect campaign snapshots\nentrypoint=check.lua\n'
    )
    (resource / "check.lua").write_text("""
local service = QuestService.get()
local quest = service:GetCampaignQuest("Quest Pack.esl", true, 0x8AB)
if quest then
    assert(quest.sequence == 23 and quest.stopped and quest.completed and quest.highestStage == 40)
    assert(#quest.stages == 21 and quest.stages[21] == 40)
    print("CAMPAIGN_SMOKE_RESTORED")
end
addEventHandler("onCampaignQuestCommitted", function(sequence, plugin, light, baseId, stage, kind, playerId)
    local current = service:GetCampaignQuest(plugin, light, baseId)
    assert(current.sequence == sequence and playerId > 0)
    if kind == 2 then
        assert(stage == 40 and current.stopped)
        print("CAMPAIGN_SMOKE_COMMITTED")
    elseif kind == 3 then
        assert(stage == 40 and current.completed)
        print("CAMPAIGN_SMOKE_COMPLETED")
    end
end)
""")
    with Runner(directory) as runner:
        runner.expect("Server started")
        test_client("wrong", "none")
        test_client("write")
        runner.expect("CAMPAIGN_SMOKE_COMMITTED")
        runner.expect("CAMPAIGN_SMOKE_COMPLETED")
        assert (directory / "shared-stock.log").read_text().endswith(' "Skyrim.esm" 0 15 5\n')
        assert (directory / "shared-npc-life.log").read_text() == '1 1 "Quest Pack.esl" 1 2201 1\n'
        assert (directory / "shared-locks.log").read_text() == '1 1 "Quest Pack.esl" 1 2221 0 0\n'
        test_client("missing")
        test_client("live")
        runner.command("/campaign-status")
        runner.expect("23 committed records")
        runner.command("/campaign-backup")
        runner.expect("Backed up 23 shared quest records")
        assert (directory / "shared-quests.log.backup").read_bytes() == (directory / "shared-quests.log").read_bytes()
    with Runner(directory) as restarted:
        restarted.expect("Loaded 23 shared quest records")
        restarted.expect("CAMPAIGN_SMOKE_RESTORED")
        test_client("restarted")
        with ExitStack() as rescuers:
            clients = []
            for role in ("death-helper", "death-distant"):
                client = rescuers.enter_context(subprocess.Popen(
                    [harness, role, version, f"127.0.0.1:{port}"], stdout=subprocess.PIPE, text=True
                ))
                assert select.select([client.stdout], [], [], 12)[0]
                assert client.stdout.readline().strip() == "DEATH_READY"
                clients.append(client)
            test_client("death-patient")
            for client in clients:
                assert client.wait(timeout=8) == 0
        with subprocess.Popen([harness, "animation-receiver", version, f"127.0.0.1:{port}"],
                              stdout=subprocess.PIPE, text=True) as receiver:
            assert select.select([receiver.stdout], [], [], 12)[0]
            assert receiver.stdout.readline().strip() == "OBJECT_ANIMATION_READY"
            test_client("animation-sender")
            assert receiver.wait(timeout=8) == 0
        with ExitStack() as passengers:
            clients = []
            for role in ("carriage-bystander", "carriage-other", "carriage-rider"):
                client = passengers.enter_context(subprocess.Popen(
                    [harness, role, version, f"127.0.0.1:{port}"], stdout=subprocess.PIPE, text=True
                ))
                assert select.select([client.stdout], [], [], 12)[0]
                assert client.stdout.readline().strip() == "CARRIAGE_READY"
                clients.append(client)
            test_client("carriage-leader")
            for client in clients:
                assert client.wait(timeout=8) == 0
        with subprocess.Popen([harness, "stock-owner", version, f"127.0.0.1:{port}"],
                              stdout=subprocess.PIPE, text=True) as owner:
            assert select.select([owner.stdout], [], [], 12)[0]
            assert owner.stdout.readline().strip() == "STOCK_OWNER_READY"
            test_client("stock-follower")
            assert owner.wait(timeout=8) == 0
        test_client("resurrect")
        assert (directory / "shared-npc-life.log").read_text().splitlines() == [
            '1 1 "Quest Pack.esl" 1 2201 1',
            '1 2 "Quest Pack.esl" 1 2201 0',
        ]
        with subprocess.Popen([harness, "weather-owner", version, f"127.0.0.1:{port}"],
                              stdout=subprocess.PIPE, text=True) as owner:
            assert select.select([owner.stdout], [], [], 12)[0]
            assert owner.stdout.readline().strip() == "WEATHER_OWNER_READY"
            test_client("weather-follower")
            assert owner.wait(timeout=8) == 0
    with Runner(directory) as restarted:
        restarted.expect("Loaded 23 shared quest records")
        test_client("verify-npc")
    with (directory / "shared-quests.log").open("ab") as journal:
        journal.write(b'3 24 "Quest Pack.esl" 1 2219 60 0')
    with Runner(directory) as damaged:
        damaged.expect("unable to load shared-quests.log (error line 24)")
        damaged.command("/campaign-status")
        damaged.expect("0 committed records")
