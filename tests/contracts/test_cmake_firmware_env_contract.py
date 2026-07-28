import json
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


ROOT = Path(__file__).resolve().parents[2]
LOADER = ROOT / "cmake" / "load_firmware_env.cmake"


@dataclass(frozen=True)
class CMakeResult:
    returncode: int
    output: str


class CMakeFirmwareEnvironmentContractTest(unittest.TestCase):
    def run_loader(self, env_text: Optional[str]) -> CMakeResult:
        with tempfile.TemporaryDirectory(
            prefix="nb-iot-cmake-env-contract-"
        ) as temporary_directory:
            fixture_root = Path(temporary_directory)
            env_file = fixture_root / ".env"
            if env_text is not None:
                env_file.write_text(env_text, encoding="utf-8")

            runner = fixture_root / "run_loader.cmake"
            runner.write_text(
                textwrap.dedent(
                    f"""\
                    include("{LOADER.as_posix()}")
                    nb_iot_load_firmware_env(
                        "{env_file.as_posix()}"
                        firmware_definitions)
                    foreach(definition IN LISTS firmware_definitions)
                        message(STATUS "FIRMWARE_DEFINITION=${{definition}}")
                    endforeach()
                    """
                ),
                encoding="utf-8",
            )

            completed = subprocess.run(
                ["cmake", "-P", str(runner)],
                cwd=ROOT,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=30,
            )
            return CMakeResult(completed.returncode, completed.stdout)

    @staticmethod
    def definitions_from(output: str) -> set[str]:
        prefix = "-- FIRMWARE_DEFINITION="
        return {
            line.removeprefix(prefix)
            for line in output.splitlines()
            if line.startswith(prefix)
        }

    def configure_symlinked_project_with_fake_env(
        self,
    ) -> Tuple[CMakeResult, List[Dict[str, object]]]:
        with tempfile.TemporaryDirectory(
            prefix="nb-iot-cmake-project-contract-"
        ) as temporary_directory:
            fixture_root = Path(temporary_directory)
            source_root = fixture_root / "source"
            build_root = fixture_root / "build"
            source_root.mkdir()

            shutil.copy2(ROOT / "CMakeLists.txt", source_root / "CMakeLists.txt")
            for entry_name in (
                "FreeRTOS-Config",
                "FreeRTOS-Kernel",
                "cmake",
                "contracts",
                "lib",
                "main.cpp",
                "pico-sdk",
                "pico_sdk_import.cmake",
                "src",
            ):
                source = ROOT / entry_name
                destination = source_root / entry_name
                destination.symlink_to(source, target_is_directory=source.is_dir())

            source_root.joinpath(".env").write_text(
                textwrap.dedent(
                    """\
                    APN_NAME=internet
                    MQTT_BROKER_HOST=p.example.test
                    MQTT_BROKER_PORT=8883
                    SUPABASE_SECRET_KEY=SUPABASE_SECRET_CANARY
                    BIZPPURIO_API_KEY=BIZPPURIO_SECRET_CANARY
                    MQTT_DEVICE_ID=MQTT_DEVICE_ID_CANARY
                    MQTT_USERNAME=MQTT_USERNAME_CANARY
                    MQTT_PASSWORD=MQTT_PASSWORD_CANARY
                    """
                ),
                encoding="utf-8",
            )

            completed = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(source_root),
                    "-B",
                    str(build_root),
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                ],
                cwd=ROOT,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=120,
            )
            result = CMakeResult(completed.returncode, completed.stdout)
            compile_commands_path = build_root / "compile_commands.json"
            if not compile_commands_path.exists():
                return result, []
            compile_commands = json.loads(
                compile_commands_path.read_text(encoding="utf-8")
            )
            return result, compile_commands

    def build_minimal_target_with_loader(self) -> CMakeResult:
        with tempfile.TemporaryDirectory(
            prefix="nb-iot-cmake-target-contract-"
        ) as temporary_directory:
            fixture_root = Path(temporary_directory)
            source_root = fixture_root / "source"
            build_root = fixture_root / "build"
            source_root.mkdir()
            source_root.joinpath(".env").write_text(
                "APN_NAME=internet\n"
                "MQTT_BROKER_HOST=p.example.test\n"
                "MQTT_BROKER_PORT=8883\n",
                encoding="utf-8",
            )
            source_root.joinpath("main.cpp").write_text(
                textwrap.dedent(
                    """\
                    #ifndef APN_NAME
                    #error APN_NAME must be defined
                    #endif
                    #ifndef MQTT_BROKER_HOST
                    #error MQTT_BROKER_HOST must be defined
                    #endif
                    #ifndef MQTT_BROKER_PORT
                    #error MQTT_BROKER_PORT must be defined
                    #endif

                    static const char* apn = APN_NAME;
                    static const char* host = MQTT_BROKER_HOST;
                    static const char* port = MQTT_BROKER_PORT;

                    int main() {
                        return apn[0] == 'i'
                            && host[0] == 'p'
                            && port[0] == '8'
                            ? 0
                            : 1;
                    }
                    """
                ),
                encoding="utf-8",
            )
            source_root.joinpath("CMakeLists.txt").write_text(
                textwrap.dedent(
                    f"""\
                    cmake_minimum_required(VERSION 3.13)
                    project(firmware_env_target_contract CXX)
                    include("{LOADER.as_posix()}")
                    nb_iot_load_firmware_env(
                        "${{CMAKE_CURRENT_SOURCE_DIR}}/.env"
                        firmware_definitions)
                    add_executable(firmware_env_target main.cpp)
                    target_compile_definitions(
                        firmware_env_target PRIVATE
                        ${{firmware_definitions}})
                    """
                ),
                encoding="utf-8",
            )

            configure = subprocess.run(
                ["cmake", "-S", str(source_root), "-B", str(build_root)],
                cwd=ROOT,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=60,
            )
            if configure.returncode != 0:
                return CMakeResult(configure.returncode, configure.stdout)

            build = subprocess.run(
                ["cmake", "--build", str(build_root)],
                cwd=ROOT,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=60,
            )
            return CMakeResult(
                build.returncode,
                configure.stdout + build.stdout,
            )

    def command_for_source(
        self,
        compile_commands: List[Dict[str, object]],
        source_suffix: str,
    ) -> str:
        matches = [
            str(entry["command"])
            for entry in compile_commands
            if str(entry["file"]).endswith(source_suffix)
        ]
        self.assertEqual(
            len(matches),
            1,
            f"expected one compile command for {source_suffix}",
        )
        return matches[0]

    def test_allowed_public_settings_are_returned_exactly(self):
        result = self.run_loader(
            'APN_NAME="internet"\n'
            "MQTT_BROKER_HOST='p.example.test'\n"
            "MQTT_BROKER_PORT=8883\n"
        )

        self.assertEqual(result.returncode, 0, "CMake loader failed")
        self.assertEqual(
            self.definitions_from(result.output),
            {
                'APN_NAME="internet"',
                'MQTT_BROKER_HOST="p.example.test"',
                'MQTT_BROKER_PORT="8883"',
            },
        )

    def test_disallowed_secrets_are_never_emitted_or_defined(self):
        result = self.run_loader(
            "APN_NAME=internet\n"
            "MQTT_BROKER_HOST=p.example.test\n"
            "MQTT_BROKER_PORT=8883\n"
            "SUPABASE_SECRET_KEY=SUPABASE_SECRET_CANARY\n"
            "BIZPPURIO_PASSWORD=BIZPPURIO_SECRET_CANARY\n"
            "MQTT_DEVICE_ID=MQTT_DEVICE_ID_CANARY\n"
            "MQTT_USERNAME=MQTT_USERNAME_CANARY\n"
            "MQTT_PASSWORD=MQTT_PASSWORD_CANARY\n"
        )

        self.assertEqual(result.returncode, 0, "CMake loader failed")
        for secret_canary in (
            "SUPABASE_SECRET_CANARY",
            "BIZPPURIO_SECRET_CANARY",
            "MQTT_DEVICE_ID_CANARY",
            "MQTT_USERNAME_CANARY",
            "MQTT_PASSWORD_CANARY",
        ):
            self.assertNotIn(secret_canary, result.output)

    def test_invalid_allowed_value_reports_only_the_key(self):
        invalid_cases = (
            ("APN_NAME", "APN_SECRET_CANARY!"),
            ("MQTT_BROKER_HOST", "HOST_SECRET_CANARY/invalid"),
            ("MQTT_BROKER_PORT", "PORT_SECRET_CANARY"),
            ("MQTT_BROKER_PORT", "70000"),
        )
        for key, secret_value in invalid_cases:
            with self.subTest(key=key, secret_value=secret_value):
                result = self.run_loader(f"{key}={secret_value}\n")

                self.assertNotEqual(result.returncode, 0)
                self.assertIn(key, result.output)
                self.assertNotIn(secret_value, result.output)

    def test_duplicate_allowed_key_reports_neither_value(self):
        result = self.run_loader(
            "APN_NAME=FIRST_SECRET_CANARY\n"
            "APN_NAME=SECOND_SECRET_CANARY\n"
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("APN_NAME", result.output)
        self.assertNotIn("FIRST_SECRET_CANARY", result.output)
        self.assertNotIn("SECOND_SECRET_CANARY", result.output)

    def test_missing_env_file_returns_no_definitions(self):
        result = self.run_loader(None)

        self.assertEqual(result.returncode, 0, "missing .env must be optional")
        self.assertEqual(self.definitions_from(result.output), set())

    def test_returned_definitions_compile_as_cxx_string_literals(self):
        result = self.build_minimal_target_with_loader()

        self.assertEqual(
            result.returncode,
            0,
            "returned definitions are not valid C++ string macros",
        )

    def test_real_project_configure_limits_definitions_to_firmware_target(self):
        result, compile_commands = (
            self.configure_symlinked_project_with_fake_env()
        )

        self.assertEqual(result.returncode, 0, "fixture configure failed")
        firmware_command = self.command_for_source(
            compile_commands, "main.cpp"
        )
        diagnostic_command = self.command_for_source(
            compile_commands, "src/diagnostics/modem_at_console.cpp"
        )
        all_compile_commands = json.dumps(compile_commands)

        for key, value in (
            ("APN_NAME", "internet"),
            ("MQTT_BROKER_HOST", "p.example.test"),
            ("MQTT_BROKER_PORT", "8883"),
        ):
            self.assertIn(key, firmware_command)
            self.assertIn(value, firmware_command)
            self.assertNotIn(key, diagnostic_command)

        for secret_canary in (
            "SUPABASE_SECRET_CANARY",
            "BIZPPURIO_SECRET_CANARY",
            "MQTT_DEVICE_ID_CANARY",
            "MQTT_USERNAME_CANARY",
            "MQTT_PASSWORD_CANARY",
        ):
            self.assertNotIn(secret_canary, all_compile_commands)
            self.assertNotIn(secret_canary, result.output)


if __name__ == "__main__":
    unittest.main()
