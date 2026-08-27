#!/usr/bin/env python3
"""Resumable depth-first orchestration for memory-partitioned rlife searches."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
from datetime import datetime, timezone
from typing import Any


MANIFEST_VERSION = 1
STATUS_REASONS = {
    "memory_cap",
    "exhausted",
    "completion",
    "halt",
    "row_limit",
    "interrupted",
}
CONTROLLED_LLSSS_OPTIONS = {
    "--save",
    "--savedir",
    "--search-name",
    "--max-memory",
    "--max-rss",
    "--status-output",
    "--partial-output",
}
RLE_KIND = re.compile(r"^#C llsss ([A-Za-z0-9_-]+)\b", re.MULTILINE)


class ManagerError(RuntimeError):
    pass


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def atomic_json(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)


def load_json(path: Path) -> dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as source:
            value = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise ManagerError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise ManagerError(f"{path} does not contain a JSON object")
    return value


class RunLock:
    def __init__(self, workdir: Path):
        self.path = workdir / "manager.lock"
        self.held = False

    def __enter__(self) -> "RunLock":
        if self.path.exists():
            try:
                pid = int(self.path.read_text(encoding="ascii").strip())
                os.kill(pid, 0)
            except (OSError, ValueError):
                self.path.unlink(missing_ok=True)
            else:
                raise ManagerError(f"another manager process (pid {pid}) holds {self.path}")
        try:
            descriptor = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        except OSError as error:
            raise ManagerError(f"cannot acquire {self.path}: {error}") from error
        with os.fdopen(descriptor, "w", encoding="ascii") as output:
            output.write(f"{os.getpid()}\n")
        self.held = True
        return self

    def __exit__(self, *_: object) -> None:
        if self.held:
            self.path.unlink(missing_ok=True)


class Manager:
    def __init__(self, workdir: Path, manifest: dict[str, Any]):
        self.workdir = workdir
        self.manifest_path = workdir / "state.json"
        self.results_path = workdir / "results.rle"
        self.events_path = workdir / "events.jsonl"
        self.manifest = manifest

    @classmethod
    def create(
        cls,
        workdir: Path,
        binary: Path,
        memory_cap: str,
        parts: int,
        boundary_slack: float,
        solver_args: list[str],
    ) -> "Manager":
        if workdir.exists() and any(workdir.iterdir()):
            raise ManagerError(f"start work directory is not empty: {workdir}")
        workdir.mkdir(parents=True, exist_ok=True)
        for directory in ("attempts", "checkpoints", "partitions"):
            (workdir / directory).mkdir()
        manifest: dict[str, Any] = {
            "version": MANIFEST_VERSION,
            "state": "running",
            "outcome": None,
            "error": None,
            "created_at": now(),
            "updated_at": now(),
            "config": {
                "binary": str(binary),
                "launch_cwd": str(Path.cwd()),
                "memory_cap": memory_cap,
                "parts": parts,
                "boundary_slack": boundary_slack,
                "solver_args": solver_args,
                "halt_w_position": None,
            },
            "next_attempt": 1,
            "next_partition": 1,
            "active": {
                "action": "extend",
                "branch": {
                    "id": "root",
                    "name": "root",
                    "checkpoint": None,
                    "height": None,
                    "depth": 0,
                    "initial": True,
                },
                "attempt": None,
            },
            "stack": [],
            "artifacts": [],
            "events": [],
            "terminal": {"exhausted": 0, "halt": 0, "completion": 0},
            "discoveries": {},
        }
        manager = cls(workdir, manifest)
        manager.save()
        manager.rebuild_outputs()
        return manager

    @classmethod
    def open(cls, workdir: Path) -> "Manager":
        manifest_path = workdir / "state.json"
        if not manifest_path.is_file():
            raise ManagerError(f"no manager state found at {manifest_path}")
        manifest = load_json(manifest_path)
        if manifest.get("version") != MANIFEST_VERSION:
            raise ManagerError(f"unsupported manager state version in {manifest_path}")
        return cls(workdir, manifest)

    def save(self) -> None:
        self.manifest["updated_at"] = now()
        atomic_json(self.manifest_path, self.manifest)

    def relative(self, path: Path) -> str:
        return os.path.relpath(path, self.workdir)

    def absolute(self, stored: str) -> Path:
        return (self.workdir / stored).resolve()

    def event(self, kind: str, **fields: Any) -> None:
        self.manifest["events"].append({"time": now(), "kind": kind, **fields})

    def add_artifact(self, artifact_id: str, path: Path, branch: str, stage: str) -> bool:
        if any(item["id"] == artifact_id for item in self.manifest["artifacts"]):
            return False
        if not path.is_file() or path.stat().st_size == 0:
            return False
        text = path.read_text(encoding="utf-8", errors="replace")
        kinds: dict[str, int] = {}
        for kind in RLE_KIND.findall(text):
            kinds[kind] = kinds.get(kind, 0) + 1
            discoveries = self.manifest["discoveries"]
            discoveries[kind] = discoveries.get(kind, 0) + 1
        self.manifest["artifacts"].append(
            {
                "id": artifact_id,
                "path": self.relative(path),
                "branch": branch,
                "stage": stage,
                "kinds": kinds,
            }
        )
        return True

    def rebuild_outputs(self) -> None:
        results_tmp = self.results_path.with_name(self.results_path.name + ".tmp")
        with results_tmp.open("w", encoding="utf-8") as output:
            for artifact in self.manifest.get("artifacts", []):
                source = self.absolute(artifact["path"])
                if not source.is_file():
                    continue
                output.write(
                    f"#C rlife-manager artifact={artifact['id']} branch={artifact['branch']} stage={artifact['stage']}\n"
                )
                data = source.read_text(encoding="utf-8", errors="replace")
                output.write(data)
                if data and not data.endswith("\n"):
                    output.write("\n")
        os.replace(results_tmp, self.results_path)

        events_tmp = self.events_path.with_name(self.events_path.name + ".tmp")
        with events_tmp.open("w", encoding="utf-8") as output:
            for event in self.manifest.get("events", []):
                output.write(json.dumps(event, sort_keys=True) + "\n")
        os.replace(events_tmp, self.events_path)

    def allocate_attempt(self, active: dict[str, Any]) -> dict[str, Any]:
        number = self.manifest["next_attempt"]
        self.manifest["next_attempt"] = number + 1
        stem = f"{number:08d}"
        attempt = {
            "id": f"extend-{stem}",
            "status": f"attempts/{stem}.status.json",
            "rle": f"attempts/{stem}.rle",
        }
        active["attempt"] = attempt
        self.save()
        return attempt

    def read_status(self, path: Path) -> dict[str, Any]:
        status = load_json(path)
        if status.get("version") != 1 or status.get("reason") not in STATUS_REASONS:
            raise ManagerError(f"invalid rlife status output: {path}")
        return status

    def remember_halt(self, status: dict[str, Any]) -> None:
        halt = status.get("halt_w_position")
        if halt is not None:
            self.manifest["config"]["halt_w_position"] = int(halt)

    def command_for_extension(self, branch: dict[str, Any], attempt: dict[str, Any]) -> list[str]:
        config = self.manifest["config"]
        command = [
            config["binary"],
            "llsss",
            "--save",
            "final",
            "--savedir",
            str((self.workdir / "checkpoints").resolve()),
            "--search-name",
            branch["name"],
            "--max-memory",
            config["memory_cap"],
            "--status-output",
            str(self.absolute(attempt["status"])),
            "--partial-output",
            str(self.absolute(attempt["rle"])),
        ]
        if branch.get("initial"):
            command.extend(config["solver_args"])
        else:
            command.extend(["--load", branch["checkpoint"]])
            halt = config.get("halt_w_position")
            if halt is not None:
                command.extend(["--halts", f"w_pos:{halt}"])
        return command

    @staticmethod
    def run_command(command: list[str], cwd: Path) -> tuple[int, bool]:
        print(f"manager: {shlex.join(command)}", flush=True)
        try:
            process = subprocess.Popen(command, cwd=cwd, start_new_session=os.name == "posix")
        except OSError as error:
            raise ManagerError(f"cannot start {command[0]}: {error}") from error
        interrupted = False
        while True:
            try:
                return process.wait(), interrupted
            except KeyboardInterrupt:
                if not interrupted:
                    print("manager: pause requested; waiting for rlife to finish its current row", file=sys.stderr, flush=True)
                    interrupted = True
                try:
                    if os.name == "posix":
                        os.killpg(process.pid, signal.SIGINT)
                    else:
                        process.send_signal(signal.SIGINT)
                except ProcessLookupError:
                    pass

    def record_status_artifact(
        self,
        artifact_id: str,
        rle_path: Path,
        branch: dict[str, Any],
        stage: str,
        status: dict[str, Any],
    ) -> bool:
        added = self.add_artifact(artifact_id, rle_path, branch["id"], stage)
        self.remember_halt(status)
        self.event(
            "solver_result",
            branch=branch["id"],
            stage=stage,
            reason=status["reason"],
            height=status.get("height"),
            completion=bool(status.get("completion")),
        )
        return added

    def fail(self, message: str) -> None:
        self.manifest["state"] = "failed"
        self.manifest["error"] = message
        self.event("manager_failed", message=message)
        self.save()
        self.rebuild_outputs()

    def drive_extension(self) -> bool:
        active = self.manifest["active"]
        branch = active["branch"]
        attempt = active.get("attempt") or self.allocate_attempt(active)
        status_path = self.absolute(attempt["status"])
        rle_path = self.absolute(attempt["rle"])
        return_code: int | None = None
        interrupted = False
        if not status_path.is_file():
            command = self.command_for_extension(branch, attempt)
            return_code, interrupted = self.run_command(command, Path(self.manifest["config"]["launch_cwd"]))

        if not status_path.is_file():
            added = self.add_artifact(attempt["id"], rle_path, branch["id"], "extend-incomplete")
            active["attempt"] = None
            if interrupted or return_code == 130:
                self.manifest["state"] = "paused"
                self.manifest["error"] = None
                self.event("manager_paused", branch=branch["id"], stage="extend")
                self.save()
                if added:
                    self.rebuild_outputs()
                return False
            self.fail(f"rlife exited with status {return_code} without writing {status_path}")
            return False

        status = self.read_status(status_path)
        if return_code not in (None, status.get("exit_status")):
            self.fail(f"rlife exit status {return_code} disagrees with {status_path}")
            return False
        added = self.record_status_artifact(attempt["id"], rle_path, branch, "extend", status)
        reason = status["reason"]
        checkpoint = status.get("checkpoint")
        was_initial = bool(branch.get("initial"))
        branch["height"] = status.get("height")
        branch["initial"] = False
        active["attempt"] = None
        if checkpoint is not None:
            if not Path(checkpoint).is_file():
                self.fail(f"status names a missing checkpoint: {checkpoint}")
                return False
            branch["checkpoint"] = checkpoint

        if reason == "memory_cap":
            if checkpoint is None:
                self.fail("memory-cap stop did not produce a checkpoint")
                return False
            active["action"] = "split"
            active["partition"] = None
            self.event("branch_split_requested", branch=branch["id"], height=branch["height"])
        elif reason == "interrupted":
            if checkpoint is None:
                if was_initial and status.get("partition_pending"):
                    # The original fingerprinted spec is still the durable
                    # work item; retrying it is safe because no restriction
                    # or row state was committed.
                    branch["initial"] = True
                else:
                    self.fail("interrupted extension did not produce a checkpoint")
                    return False
            self.manifest["state"] = "paused"
            self.manifest["error"] = None
            self.event("manager_paused", branch=branch["id"], stage="extend")
        elif reason == "completion":
            self.manifest["terminal"]["completion"] += 1
            self.manifest["state"] = "complete"
            self.manifest["outcome"] = "completion"
            self.manifest["active"] = None
        elif reason in ("exhausted", "halt"):
            self.manifest["terminal"][reason] += 1
            self.manifest["active"] = None
        else:
            self.fail(f"unexpected extension result: {reason}")
            return False

        self.save()
        if added:
            self.rebuild_outputs()
        if interrupted and self.manifest["state"] == "running":
            self.manifest["state"] = "paused"
            self.event("manager_paused", branch=branch["id"], stage="extend")
            self.save()
        return self.manifest["state"] == "running"

    def allocate_partition(self, active: dict[str, Any]) -> dict[str, Any]:
        number = self.manifest["next_partition"]
        self.manifest["next_partition"] = number + 1
        partition = {
            "id": f"partition-{number:08d}",
            "name": f"p{number:08d}",
            "directory": f"partitions/{number:08d}",
        }
        active["partition"] = partition
        self.save()
        return partition

    def partition_statuses(self, partition: dict[str, Any]) -> list[tuple[str, Path, Path, dict[str, Any]]] | None:
        count = int(self.manifest["config"]["parts"])
        directory = self.absolute(partition["directory"])
        statuses: list[tuple[str, Path, Path, dict[str, Any]]] = []
        for part in range(1, count + 1):
            name = f"{partition['name']}-{part}"
            status_path = directory / f"{name}.status.json"
            rle_path = directory / f"{name}.rle"
            if not status_path.is_file():
                return None
            status = self.read_status(status_path)
            if status["reason"] == "interrupted":
                return None
            checkpoint = status.get("checkpoint")
            if checkpoint is None or not Path(checkpoint).is_file():
                return None
            statuses.append((name, status_path, rle_path, status))
        return statuses

    def command_for_partition(self, branch: dict[str, Any], partition: dict[str, Any]) -> list[str]:
        config = self.manifest["config"]
        directory = self.absolute(partition["directory"])
        directory.mkdir(parents=True, exist_ok=True)
        return [
            config["binary"],
            "partition",
            "--load",
            branch["checkpoint"],
            "--parts",
            str(config["parts"]),
            "--search-name",
            partition["name"],
            "--output",
            str(directory),
            "--boundary-slack",
            str(config["boundary_slack"]),
            "--materialize",
            "--force",
            "--",
            "--max-memory",
            "none",
            "--status-output",
            str(directory / "{name}.status.json"),
            "--partial-output",
            str(directory / "{name}.rle"),
        ]

    def drive_partition(self) -> bool:
        active = self.manifest["active"]
        branch = active["branch"]
        partition = active.get("partition") or self.allocate_partition(active)
        statuses = self.partition_statuses(partition)
        return_code: int | None = None
        interrupted = False
        if statuses is None:
            command = self.command_for_partition(branch, partition)
            return_code, interrupted = self.run_command(command, Path(self.manifest["config"]["launch_cwd"]))
            statuses = self.partition_statuses(partition)

        if statuses is None:
            if interrupted or return_code == 130:
                self.manifest["state"] = "paused"
                self.manifest["error"] = None
                self.event("manager_paused", branch=branch["id"], stage="partition")
                self.save()
                return False
            self.fail(f"partition materialization failed with status {return_code}; resume retries it with --force")
            return False
        if return_code not in (None, 0):
            self.fail(f"partition materialization returned status {return_code} despite complete child outputs")
            return False

        children: list[dict[str, Any]] = []
        completion_halt = False
        artifacts_added = False
        for part, (name, _, rle_path, status) in enumerate(statuses, start=1):
            child_id = f"{partition['id']}/{part}"
            child = {
                "id": child_id,
                "name": name,
                "checkpoint": status["checkpoint"],
                "height": status.get("height"),
                "depth": int(branch["depth"]) + 1,
                "initial": False,
            }
            artifacts_added |= self.record_status_artifact(
                f"{partition['id']}-part-{part}", rle_path, child, "materialize", status
            )
            reason = status["reason"]
            if reason == "row_limit" or reason == "memory_cap":
                children.append(child)
            elif reason == "exhausted":
                self.manifest["terminal"]["exhausted"] += 1
            elif reason == "halt":
                self.manifest["terminal"]["halt"] += 1
            elif reason == "completion":
                self.manifest["terminal"]["completion"] += 1
                completion_halt = True
            else:
                self.fail(f"unexpected materialization result for {name}: {reason}")
                return False

        # Reverse insertion makes the first materialized part the next LIFO
        # branch, preserving deterministic depth-first traversal.
        for child in reversed(children):
            self.manifest["stack"].append(child)
        self.event(
            "partition_materialized",
            branch=branch["id"],
            partition=partition["id"],
            children=len(children),
        )
        self.manifest["active"] = None
        if completion_halt:
            self.manifest["state"] = "complete"
            self.manifest["outcome"] = "completion"
        elif interrupted:
            self.manifest["state"] = "paused"
            self.event("manager_paused", branch=branch["id"], stage="partition")
        self.save()
        if artifacts_added:
            self.rebuild_outputs()
        return self.manifest["state"] == "running"

    def finish_if_done(self) -> bool:
        if self.manifest["active"] is not None or self.manifest["stack"]:
            return False
        terminal = self.manifest["terminal"]
        self.manifest["state"] = "complete"
        self.manifest["outcome"] = "halt" if terminal["halt"] else "exhausted"
        self.event("manager_complete", outcome=self.manifest["outcome"])
        self.save()
        self.rebuild_outputs()
        return True

    def run(self) -> int:
        self.rebuild_outputs()
        if self.manifest["state"] == "complete":
            self.print_status()
            return 0
        self.manifest["state"] = "running"
        self.manifest["error"] = None
        active = self.manifest.get("active")
        if active and active["action"] == "extend" and active.get("attempt"):
            attempt_status = self.absolute(active["attempt"]["status"])
            if not attempt_status.is_file():
                active["attempt"] = None
        self.save()

        while self.manifest["state"] == "running":
            if self.manifest["active"] is None:
                if not self.manifest["stack"]:
                    self.finish_if_done()
                    break
                branch = self.manifest["stack"].pop()
                self.manifest["active"] = {"action": "extend", "branch": branch, "attempt": None}
                self.event("branch_started", branch=branch["id"], height=branch.get("height"))
                self.save()
            action = self.manifest["active"]["action"]
            if action == "extend":
                if not self.drive_extension():
                    break
            elif action == "split":
                if not self.drive_partition():
                    break
            else:
                self.fail(f"unknown active manager action: {action}")
                break

        self.print_status()
        if self.manifest["state"] == "complete":
            return 0
        if self.manifest["state"] == "paused":
            return 130
        return 1

    def print_status(self) -> None:
        active = self.manifest.get("active")
        active_text = "none"
        if active:
            active_text = f"{active['branch']['id']} ({active['action']}, height={active['branch'].get('height')})"
        print(
            f"manager state={self.manifest['state']} outcome={self.manifest.get('outcome')} "
            f"active={active_text} queued={len(self.manifest.get('stack', []))}"
        )
        print(
            f"terminal={self.manifest.get('terminal', {})} discoveries={self.manifest.get('discoveries', {})}"
        )
        if self.manifest.get("error"):
            print(f"error: {self.manifest['error']}", file=sys.stderr)
        print(f"state: {self.manifest_path}")
        print(f"combined partials/completions: {self.results_path}")


def validate_start_arguments(arguments: list[str]) -> list[str]:
    if arguments and arguments[0] == "--":
        arguments = arguments[1:]
    if arguments and arguments[0] == "llsss":
        arguments = arguments[1:]
    if not arguments:
        raise ManagerError("start requires llsss arguments after --")
    for argument in arguments:
        name = argument.split("=", 1)[0]
        if name in CONTROLLED_LLSSS_OPTIONS:
            raise ManagerError(f"the manager controls llsss option {name}")
    return arguments


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run an rlife search depth-first, recursively materializing partitions at a soft memory cap."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    start = commands.add_parser(
        "start",
        help="create and run a managed search",
        description="Create a managed search. Follow these options with -- and the ordinary llsss arguments.",
        epilog="example: rlife_manager.py start run --max-memory 32GiB -- --halts w_pos:300 c5-f2b '@bg(11)'",
    )
    start.add_argument("workdir", type=Path)
    start.add_argument("--binary", type=Path, default=Path("./rlife"))
    start.add_argument("--max-memory", required=True, help="rlife soft cap, for example 32GiB")
    start.add_argument("--parts", type=int, default=4)
    start.add_argument("--boundary-slack", type=float, default=1.0)
    resume = commands.add_parser("resume", help="resume a paused or failed managed search")
    resume.add_argument("workdir", type=Path)
    status = commands.add_parser("status", help="show persistent managed-search state")
    status.add_argument("workdir", type=Path)
    return parser


def main() -> int:
    parser = build_parser()
    raw_arguments = sys.argv[1:]
    solver_arguments: list[str] = []
    if "--" in raw_arguments:
        separator = raw_arguments.index("--")
        solver_arguments = raw_arguments[separator + 1 :]
        raw_arguments = raw_arguments[:separator]
    args = parser.parse_args(raw_arguments)
    try:
        workdir = args.workdir.expanduser().resolve()
        if args.command == "status":
            Manager.open(workdir).print_status()
            return 0
        if args.command == "start":
            if args.parts < 2:
                raise ManagerError("--parts must be at least 2")
            if not (0.0 <= args.boundary_slack < 50.0):
                raise ManagerError("--boundary-slack must be in [0,50)")
            binary = args.binary.expanduser().resolve()
            if not binary.is_file() or not os.access(binary, os.X_OK):
                raise ManagerError(f"rlife binary is not executable: {binary}")
            solver_args = validate_start_arguments(solver_arguments)
            manager = Manager.create(
                workdir,
                binary,
                args.max_memory,
                args.parts,
                args.boundary_slack,
                solver_args,
            )
        else:
            if solver_arguments:
                raise ManagerError("only start accepts llsss arguments after --")
            manager = Manager.open(workdir)
        with RunLock(workdir):
            try:
                return manager.run()
            except ManagerError as error:
                if manager.manifest.get("state") != "failed":
                    manager.fail(str(error))
                raise
    except ManagerError as error:
        print(f"rlife-manager: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
