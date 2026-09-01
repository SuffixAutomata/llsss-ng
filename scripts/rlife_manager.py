#!/usr/bin/env python3
"""Resumable depth-first orchestration for memory-partitioned rlife searches."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
from datetime import datetime, timezone
from typing import Any


MANIFEST_VERSION = 1
DISK_PAUSE_EXIT = 75
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


def parse_byte_size(text: str, option: str) -> int:
    value = text.lower()
    if value in ("none", "off", "0"):
        return 0
    match = re.fullmatch(r"([0-9]+)([a-z]*)", value)
    suffixes = {
        "": 1,
        "b": 1,
        "k": 1 << 10,
        "kb": 1 << 10,
        "kib": 1 << 10,
        "m": 1 << 20,
        "mb": 1 << 20,
        "mib": 1 << 20,
        "g": 1 << 30,
        "gb": 1 << 30,
        "gib": 1 << 30,
        "t": 1 << 40,
        "tb": 1 << 40,
        "tib": 1 << 40,
    }
    if match is None or match.group(2) not in suffixes:
        raise ManagerError(f"{option} must be none or an integer byte size such as 500MiB or 8GiB")
    number = int(match.group(1))
    result = number * suffixes[match.group(2)]
    if number == 0 or result >= 1 << 64:
        raise ManagerError(f"{option} is outside the supported range")
    return result


def format_bytes(value: int) -> str:
    for suffix, unit in (("TiB", 1 << 40), ("GiB", 1 << 30), ("MiB", 1 << 20), ("KiB", 1 << 10)):
        if value >= unit:
            return f"{value / unit:.2f}{suffix}"
    return f"{value}B"


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
        config = self.manifest.setdefault("config", {})
        if "disk_reserve_bytes" not in config:
            memory_cap = str(config.get("memory_cap", "none"))
            config["disk_reserve"] = memory_cap
            config["disk_reserve_bytes"] = parse_byte_size(memory_cap, "saved memory cap")
        else:
            config.setdefault("disk_reserve", str(config["disk_reserve_bytes"]))
        config.setdefault("archive_dir", None)
        self.manifest.setdefault("pause_reason", None)
        self.manifest.setdefault("archived_checkpoints", [])

    @classmethod
    def create(
        cls,
        workdir: Path,
        binary: Path,
        memory_cap: str,
        disk_reserve: str | None,
        archive_dir: Path | None,
        parts: int,
        boundary_slack: float,
        solver_args: list[str],
    ) -> "Manager":
        if archive_dir is not None:
            try:
                archive_dir.relative_to(workdir)
            except ValueError:
                pass
            else:
                raise ManagerError("--archive-dir must be outside the managed work directory")
            try:
                workdir.relative_to(archive_dir)
            except ValueError:
                pass
            else:
                raise ManagerError("--archive-dir must not contain the managed work directory")
        if workdir.exists() and any(workdir.iterdir()):
            raise ManagerError(f"start work directory is not empty: {workdir}")
        workdir.mkdir(parents=True, exist_ok=True)
        for directory in ("attempts", "checkpoints", "partitions"):
            (workdir / directory).mkdir()
        reserve_text = memory_cap if disk_reserve is None else disk_reserve
        reserve_bytes = parse_byte_size(reserve_text, "--disk-reserve")
        manifest: dict[str, Any] = {
            "version": MANIFEST_VERSION,
            "state": "running",
            "outcome": None,
            "error": None,
            "pause_reason": None,
            "created_at": now(),
            "updated_at": now(),
            "config": {
                "binary": str(binary),
                "launch_cwd": str(Path.cwd()),
                "memory_cap": memory_cap,
                "disk_reserve": reserve_text,
                "disk_reserve_bytes": reserve_bytes,
                "archive_dir": str(archive_dir) if archive_dir is not None else None,
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
            "archived_checkpoints": [],
        }
        manager = cls(workdir, manifest)
        manager.save()
        manager.rebuild_outputs_best_effort()
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
        try:
            atomic_json(self.manifest_path, self.manifest)
        except OSError as error:
            raise ManagerError(f"cannot save manager state {self.manifest_path}: {error}") from error

    def relative(self, path: Path) -> str:
        return os.path.relpath(path, self.workdir)

    def absolute(self, stored: str) -> Path:
        return (self.workdir / stored).resolve()

    def configure(self, disk_reserve: str | None, archive_dir: Path | None) -> bool:
        changed = False
        config = self.manifest["config"]
        if disk_reserve is not None:
            reserve_bytes = parse_byte_size(disk_reserve, "--disk-reserve")
            config["disk_reserve"] = disk_reserve
            config["disk_reserve_bytes"] = reserve_bytes
            changed = True
        if archive_dir is not None:
            resolved = archive_dir.expanduser().resolve()
            try:
                resolved.relative_to(self.workdir)
            except ValueError:
                pass
            else:
                raise ManagerError("--archive-dir must be outside the managed work directory")
            try:
                self.workdir.relative_to(resolved)
            except ValueError:
                pass
            else:
                raise ManagerError("--archive-dir must not contain the managed work directory")
            config["archive_dir"] = str(resolved)
            changed = True
        return changed

    def disk_free_bytes(self) -> int:
        try:
            return shutil.disk_usage(self.workdir).free
        except OSError as error:
            raise ManagerError(f"cannot inspect free space for {self.workdir}: {error}") from error

    def live_checkpoint_paths(self) -> set[Path]:
        result: set[Path] = set()
        active = self.manifest.get("active")
        branches = list(self.manifest.get("stack", []))
        if active is not None:
            branches.append(active["branch"])
        for branch in branches:
            checkpoint = branch.get("checkpoint")
            if checkpoint:
                result.add(Path(checkpoint).resolve())
        return result

    def retired_checkpoint_paths(self) -> list[Path]:
        live = self.live_checkpoint_paths()
        active_partition: Path | None = None
        active = self.manifest.get("active")
        if active is not None and active.get("action") == "split" and active.get("partition"):
            active_partition = self.absolute(active["partition"]["directory"])

        candidates: set[Path] = set()
        status_paths = list((self.workdir / "attempts").glob("*.status.json"))
        status_paths.extend((self.workdir / "partitions").glob("*/*.status.json"))
        for status_path in status_paths:
            try:
                checkpoint = load_json(status_path).get("checkpoint")
            except ManagerError:
                continue
            if not checkpoint:
                continue
            path = Path(checkpoint).resolve()
            try:
                path.relative_to(self.workdir)
            except ValueError:
                continue
            if path in live or not path.is_file() or path.name.endswith(".tmp"):
                continue
            if active_partition is not None:
                try:
                    path.relative_to(active_partition)
                except ValueError:
                    pass
                else:
                    continue
            candidates.add(path)
        return sorted(candidates, key=lambda path: (path.stat().st_mtime_ns, str(path)))

    @staticmethod
    def same_file_contents(first: Path, second: Path) -> bool:
        if first.stat().st_size != second.stat().st_size:
            return False
        first_digest = hashlib.sha256()
        second_digest = hashlib.sha256()
        with first.open("rb") as left, second.open("rb") as right:
            while True:
                left_block = left.read(16 << 20)
                right_block = right.read(16 << 20)
                if not left_block and not right_block:
                    break
                first_digest.update(left_block)
                second_digest.update(right_block)
        return first_digest.digest() == second_digest.digest()

    def archive_checkpoint(self, source: Path, archive_root: Path) -> int:
        relative = source.relative_to(self.workdir)
        destination = archive_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        size = source.stat().st_size
        record = {
            "source": str(relative),
            "destination": str(destination),
            "bytes": size,
            "time": now(),
            "state": "planned",
        }
        records = self.manifest["archived_checkpoints"]
        existing = next(
            (
                item
                for item in records
                if item.get("source") == record["source"] and item.get("destination") == record["destination"]
            ),
            None,
        )
        if existing is None:
            records.append(record)
        else:
            record = existing
            record.update({"bytes": size, "time": now(), "state": "planned"})

        if destination.exists():
            if not destination.is_file() or not self.same_file_contents(source, destination):
                raise ManagerError(f"archive destination conflicts with checkpoint: {destination}")
        elif source.stat().st_dev == destination.parent.stat().st_dev:
            os.replace(source, destination)
        else:
            temporary = destination.with_name(destination.name + f".moving-{os.getpid()}")
            try:
                with source.open("rb") as input_file, temporary.open("wb") as output_file:
                    shutil.copyfileobj(input_file, output_file, length=16 << 20)
                    output_file.flush()
                    os.fsync(output_file.fileno())
                shutil.copystat(source, temporary)
                if temporary.stat().st_size != size:
                    raise ManagerError(f"archived checkpoint has the wrong size: {temporary}")
                os.replace(temporary, destination)
            except Exception:
                temporary.unlink(missing_ok=True)
                raise
            source.unlink()

        if source.exists():
            source.unlink()
        record["state"] = "archived"
        record["time"] = now()
        self.event(
            "checkpoint_archived",
            source=str(relative),
            destination=str(destination),
            bytes=size,
        )
        self.save()
        print(f"manager: archived retired checkpoint {relative} ({format_bytes(size)}) -> {destination}", flush=True)
        return size

    def archive_retired_checkpoints(self, target_free: int | None = None) -> tuple[int, int]:
        configured = self.manifest["config"].get("archive_dir")
        if not configured:
            return self.disk_free_bytes(), 0
        archive_root = Path(configured)
        archive_root.mkdir(parents=True, exist_ok=True)
        if target_free is not None and archive_root.stat().st_dev == self.workdir.stat().st_dev:
            print(
                f"manager: archive directory {archive_root} is on the managed filesystem and cannot reclaim space",
                file=sys.stderr,
                flush=True,
            )
            return self.disk_free_bytes(), 0

        moved = 0
        for source in self.retired_checkpoint_paths():
            moved += self.archive_checkpoint(source, archive_root)
            free = self.disk_free_bytes()
            if target_free is not None and free >= target_free:
                return free, moved
        return self.disk_free_bytes(), moved

    def pause_for_disk(self, stage: str, free: int, reserve: int, detail: str | None = None) -> None:
        message = (
            f"disk reserve reached before {stage}: {format_bytes(free)} free is below "
            f"the configured {format_bytes(reserve)} reserve"
        )
        if detail:
            message += f" ({detail})"
        self.manifest["state"] = "paused"
        self.manifest["error"] = message
        self.manifest["pause_reason"] = {
            "kind": "disk_space",
            "stage": stage,
            "free_bytes": free,
            "required_bytes": reserve,
        }
        self.event(
            "manager_paused_disk",
            stage=stage,
            free_bytes=free,
            required_bytes=reserve,
            detail=detail,
        )
        self.save()
        print(f"manager: {message}; free space and resume", file=sys.stderr, flush=True)

    def ensure_disk_space(self, stage: str) -> bool:
        reserve = int(self.manifest["config"].get("disk_reserve_bytes", 0))
        if reserve == 0:
            return True
        free = self.disk_free_bytes()
        if free >= reserve:
            return True
        detail: str | None = None
        if self.manifest["config"].get("archive_dir"):
            try:
                free, moved = self.archive_retired_checkpoints(reserve)
                if moved:
                    print(
                        f"manager: archive reclaimed {format_bytes(moved)}; {format_bytes(free)} now free",
                        flush=True,
                    )
            except (ManagerError, OSError) as error:
                detail = f"automatic archival failed: {error}"
                free = self.disk_free_bytes()
        if free >= reserve:
            return True
        self.pause_for_disk(stage, free, reserve, detail)
        return False

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

    def rebuild_outputs_best_effort(self) -> bool:
        try:
            self.rebuild_outputs()
            return True
        except OSError as error:
            print(f"manager: could not rebuild summary outputs: {error}", file=sys.stderr, flush=True)
            return False

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
        self.manifest["pause_reason"] = None
        self.event("manager_failed", message=message)
        self.save()
        self.rebuild_outputs_best_effort()

    def drive_extension(self) -> bool:
        active = self.manifest["active"]
        branch = active["branch"]
        attempt = active.get("attempt") or self.allocate_attempt(active)
        status_path = self.absolute(attempt["status"])
        rle_path = self.absolute(attempt["rle"])
        return_code: int | None = None
        interrupted = False
        if not status_path.is_file():
            if not self.ensure_disk_space(f"extending {branch['id']}"):
                return False
            command = self.command_for_extension(branch, attempt)
            return_code, interrupted = self.run_command(command, Path(self.manifest["config"]["launch_cwd"]))

        if not status_path.is_file():
            added = self.add_artifact(attempt["id"], rle_path, branch["id"], "extend-incomplete")
            active["attempt"] = None
            if interrupted or return_code == 130:
                self.manifest["state"] = "paused"
                self.manifest["error"] = None
                self.manifest["pause_reason"] = {"kind": "interrupt", "stage": "extend"}
                self.event("manager_paused", branch=branch["id"], stage="extend")
                self.save()
                if added:
                    self.rebuild_outputs_best_effort()
                return False
            reserve = int(self.manifest["config"].get("disk_reserve_bytes", 0))
            if reserve and self.disk_free_bytes() < reserve:
                self.save()
                if self.ensure_disk_space(f"retrying {branch['id']} after an incomplete checkpoint"):
                    if added:
                        self.rebuild_outputs_best_effort()
                    return True
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
            self.manifest["pause_reason"] = {"kind": "interrupt", "stage": "extend"}
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
        if interrupted and self.manifest["state"] == "running":
            self.manifest["state"] = "paused"
            self.manifest["pause_reason"] = {"kind": "interrupt", "stage": "extend"}
            self.event("manager_paused", branch=branch["id"], stage="extend")
            self.save()
        if self.manifest["state"] == "running" and not self.ensure_disk_space(f"continuing after {branch['id']}"):
            return False
        if added:
            self.rebuild_outputs_best_effort()
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

    def partition_status(
        self, partition: dict[str, Any], part: int
    ) -> tuple[str, Path, Path, dict[str, Any]] | None:
        directory = self.absolute(partition["directory"])
        name = f"{partition['name']}-{part}"
        status_path = directory / f"{name}.status.json"
        rle_path = directory / f"{name}.rle"
        if not status_path.is_file():
            return None
        try:
            status = self.read_status(status_path)
        except ManagerError as error:
            print(f"manager: ignoring incomplete partition status {status_path}: {error}", file=sys.stderr)
            return None
        active = self.manifest.get("active")
        expected_height = int(active["branch"]["height"]) + 1 if active is not None else None
        if (
            status.get("search_name") != name
            or status["reason"] == "interrupted"
            or status.get("exit_status") != 0
            or status.get("height") != expected_height
        ):
            return None
        checkpoint = status.get("checkpoint")
        expected_checkpoint = (directory / f"{name}_{expected_height}").resolve()
        if (
            checkpoint is None
            or Path(checkpoint).resolve() != expected_checkpoint
            or not expected_checkpoint.is_file()
        ):
            return None
        return name, status_path, rle_path, status

    def partition_statuses(
        self, partition: dict[str, Any]
    ) -> list[tuple[str, Path, Path, dict[str, Any]]] | None:
        statuses: list[tuple[str, Path, Path, dict[str, Any]]] = []
        for part in range(1, int(self.manifest["config"]["parts"]) + 1):
            status = self.partition_status(partition, part)
            if status is None:
                return None
            statuses.append(status)
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
        while statuses is None:
            if not self.ensure_disk_space(f"materializing {partition['id']}"):
                return False
            command = self.command_for_partition(branch, partition)
            return_code, interrupted = self.run_command(
                command, Path(self.manifest["config"]["launch_cwd"])
            )
            statuses = self.partition_statuses(partition)
            if interrupted or return_code == 130:
                self.manifest["state"] = "paused"
                self.manifest["error"] = None
                self.manifest["pause_reason"] = {"kind": "interrupt", "stage": "partition"}
                self.event("manager_paused", branch=branch["id"], stage="partition")
                self.save()
                return False
            if statuses is None:
                reserve = int(self.manifest["config"].get("disk_reserve_bytes", 0))
                if reserve and self.disk_free_bytes() < reserve:
                    if self.ensure_disk_space(f"retrying {partition['id']} after an incomplete materialization"):
                        continue
                    return False
                self.fail(
                    f"partition materialization failed with status {return_code}; "
                    "resume retries the split with --force"
                )
                return False
            if return_code != 0:
                self.fail(
                    f"partition materialization returned status {return_code} despite complete child outputs"
                )
                return False
            if not self.ensure_disk_space(f"continuing after {partition['id']}"):
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
        self.save()
        if artifacts_added:
            self.rebuild_outputs_best_effort()
        return self.manifest["state"] == "running"

    def finish_if_done(self) -> bool:
        if self.manifest["active"] is not None or self.manifest["stack"]:
            return False
        terminal = self.manifest["terminal"]
        self.manifest["state"] = "complete"
        self.manifest["outcome"] = "halt" if terminal["halt"] else "exhausted"
        self.event("manager_complete", outcome=self.manifest["outcome"])
        self.save()
        self.rebuild_outputs_best_effort()
        return True

    def run(self) -> int:
        if self.manifest["state"] == "complete":
            self.rebuild_outputs_best_effort()
            self.print_status()
            return 0
        self.manifest["state"] = "running"
        self.manifest["error"] = None
        self.manifest["pause_reason"] = None
        active = self.manifest.get("active")
        if active and active["action"] == "extend" and active.get("attempt"):
            attempt_status = self.absolute(active["attempt"]["status"])
            if not attempt_status.is_file():
                active["attempt"] = None
        self.save()
        if not self.ensure_disk_space("starting the next search operation"):
            self.print_status()
            return DISK_PAUSE_EXIT
        self.rebuild_outputs_best_effort()

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
            if (self.manifest.get("pause_reason") or {}).get("kind") == "disk_space":
                return DISK_PAUSE_EXIT
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
        reserve = int(self.manifest["config"].get("disk_reserve_bytes", 0))
        print(
            f"disk_free={format_bytes(self.disk_free_bytes())} disk_reserve={format_bytes(reserve)} "
            f"archive={self.manifest['config'].get('archive_dir') or 'none'}"
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
    start.add_argument(
        "--disk-reserve",
        help="minimum free space before launching more work (default: --max-memory)",
    )
    start.add_argument(
        "--archive-dir",
        type=Path,
        help="move retired checkpoint payloads here when the disk reserve is reached",
    )
    start.add_argument("--parts", type=int, default=4)
    start.add_argument("--boundary-slack", type=float, default=1.0)
    resume = commands.add_parser("resume", help="resume a paused or failed managed search")
    resume.add_argument("workdir", type=Path)
    resume.add_argument("--disk-reserve", help="replace the saved minimum-free-space setting")
    resume.add_argument("--archive-dir", type=Path, help="set or replace the saved archive directory")
    status = commands.add_parser("status", help="show persistent managed-search state")
    status.add_argument("workdir", type=Path)
    configure = commands.add_parser("configure", help="change durable disk-space settings without resuming")
    configure.add_argument("workdir", type=Path)
    configure.add_argument("--disk-reserve", help="replace the saved minimum-free-space setting")
    configure.add_argument("--archive-dir", type=Path, help="set or replace the saved archive directory")
    archive = commands.add_parser("archive", help="move all currently retired checkpoints to the archive")
    archive.add_argument("workdir", type=Path)
    archive.add_argument("--archive-dir", type=Path, help="set or replace the saved archive directory")
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
            parse_byte_size(args.max_memory, "--max-memory")
            binary = args.binary.expanduser().resolve()
            if not binary.is_file() or not os.access(binary, os.X_OK):
                raise ManagerError(f"rlife binary is not executable: {binary}")
            solver_args = validate_start_arguments(solver_arguments)
            archive_dir = args.archive_dir.expanduser().resolve() if args.archive_dir is not None else None
            manager = Manager.create(
                workdir,
                binary,
                args.max_memory,
                args.disk_reserve,
                archive_dir,
                args.parts,
                args.boundary_slack,
                solver_args,
            )
        else:
            if solver_arguments:
                raise ManagerError("only start accepts llsss arguments after --")
            manager = Manager.open(workdir)
        with RunLock(workdir):
            if args.command == "configure":
                if args.disk_reserve is None and args.archive_dir is None:
                    raise ManagerError("configure requires --disk-reserve and/or --archive-dir")
                manager.configure(args.disk_reserve, args.archive_dir)
                manager.save()
                manager.print_status()
                return 0
            if args.command == "archive":
                manager.configure(None, args.archive_dir)
                if not manager.manifest["config"].get("archive_dir"):
                    raise ManagerError("archive requires --archive-dir or a previously configured archive directory")
                manager.save()
                _, moved = manager.archive_retired_checkpoints()
                print(f"manager: archived {format_bytes(moved)} of retired checkpoints")
                manager.print_status()
                return 0
            if args.command == "resume" and manager.configure(args.disk_reserve, args.archive_dir):
                manager.save()
            try:
                return manager.run()
            except (ManagerError, OSError) as error:
                if manager.manifest.get("state") not in ("failed", "paused", "complete"):
                    manager.fail(str(error))
                raise
    except (ManagerError, OSError) as error:
        print(f"rlife-manager: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
