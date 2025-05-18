import subprocess
import os
import re
import sys
import argparse

# Define paths
UNREAL_ENGINE_PATH = "D:\\UnrealEngine"
WARTRIBES_PROJECT_PATH = "D:\\Wartribes\\WarTribes.uproject"
BUILD_BATCH_FILE = os.path.join(UNREAL_ENGINE_PATH, "Engine", "Build", "BatchFiles", "Build.bat")
UNREAL_EDITOR_CMD = os.path.join(UNREAL_ENGINE_PATH, "Engine", "Binaries", "Win64", "UnrealEditor-Win64-DebugGame-Cmd.exe")

BUILD_OUTPUT_FILE = "BuildOutput.txt"
TEST_OUTPUT_FILE = "TestOutput.txt"

# Define test categories and their full automation paths for SimpleGameplayAbilitySystem
TEST_CATEGORIES = {
    "attributes": [
        "GameTests.SGAS.Attributes.BasicManipulation",
        "GameTests.SGAS.Attributes.StructManipulation"
    ],
    "minimal": [
        "GameTests.SGAS.Minimal.JustAdd"
    ],
    "all": [
        "GameTests.SGAS.Attributes.BasicManipulation",
        "GameTests.SGAS.Attributes.StructManipulation",
        "GameTests.SGAS.Minimal.JustAdd"
    ]
}

def run_build():
    print("Starting build process...")
    build_command = [
        BUILD_BATCH_FILE,
        "WarTribesEditor",
        "Win64",
        "DebugGame",
        f"-Project={WARTRIBES_PROJECT_PATH}"
    ]

    try:
        with open(BUILD_OUTPUT_FILE, "w") as f:
            process = subprocess.run(build_command, stdout=f, stderr=f, text=True, check=False)

        with open(BUILD_OUTPUT_FILE, "r") as f:
            build_output = f.readlines()

        error_patterns = [
            re.compile(r"error C\d+", re.IGNORECASE),  # C++ compiler errors
            re.compile(r"Error: Type '.*' is not supported by blueprint", re.IGNORECASE), # UHT blueprint errors
            re.compile(r"Result: Failed", re.IGNORECASE), # Generic build tool failure
            re.compile(r"fatal error", re.IGNORECASE),
            re.compile(r"failed to compile", re.IGNORECASE),
            re.compile(r"build failed", re.IGNORECASE),
            re.compile(r"unresolved external symbol", re.IGNORECASE)
        ]
        
        error_lines = []
        for line in build_output:
            if any(pattern.search(line) for pattern in error_patterns):
                error_lines.append(line)

        if error_lines:
            print(f"Build failed, see {BUILD_OUTPUT_FILE}")
            return False
        else:
            print("Build completed successfully.")
            return True

    except Exception as e:
        print(f"An error occurred during build: {e}")
        return False

def run_tests(test_names):
    if os.path.exists(TEST_OUTPUT_FILE):
        os.remove(TEST_OUTPUT_FILE)
    all_tests_passed = True
    for test_name in test_names:
        print(f"Starting test process for: {test_name}...")
        test_command = [
            UNREAL_EDITOR_CMD,
            WARTRIBES_PROJECT_PATH,
            f"-execcmds=Automation RunTests {test_name};Quit",
            "-stdout",
            "-unattended",
            "-NOSPLASH",
            "-AllowStdOutLogVerbosity",
            "-NullRHI"
        ]

        try:
            process = subprocess.run(test_command, capture_output=True, text=True, check=False)
            test_output = process.stdout.splitlines(keepends=True)

            print("--- Test Results (Filtered) ---")
            filtered_lines = []
            test_filter_patterns = [
                re.compile(r"LogAutomationTest", re.IGNORECASE),
                re.compile(r"LogInit", re.IGNORECASE),
                re.compile(r"LogExit", re.IGNORECASE),
                re.compile(r"LogTemp", re.IGNORECASE),
                re.compile(r"Error", re.IGNORECASE),
                re.compile(r"Warning", re.IGNORECASE),
                re.compile(r"Failed", re.IGNORECASE),
                re.compile(r"Passed", re.IGNORECASE),
                re.compile(r"Total", re.IGNORECASE),
                re.compile(r"Ran", re.IGNORECASE),
                re.compile(r"Skipped", re.IGNORECASE),
                re.compile(r"Automation Test Succeeded", re.IGNORECASE),
                re.compile(r"Automation Test Failed", re.IGNORECASE)
            ]
            
            blocklist_patterns = [
                re.compile(r"package was marked as deleted in editor", re.IGNORECASE),
                re.compile(r"LogSavePackage", re.IGNORECASE),
                re.compile(r"LogLoad", re.IGNORECASE),
                re.compile(r"LogTargetPlatformManager", re.IGNORECASE),
                re.compile(r"MapCheck", re.IGNORECASE),
                re.compile(r"LogInit", re.IGNORECASE),
                re.compile(r"LogClass", re.IGNORECASE),
                re.compile(r"LogBlueprint", re.IGNORECASE),
                re.compile(r"LoadErrors", re.IGNORECASE),
                re.compile(r"LogSlate", re.IGNORECASE),
                re.compile(r"LogAudio", re.IGNORECASE),
                re.compile(r"LogGameplayTags", re.IGNORECASE),
                re.compile(r"LogUnrealEdMisc", re.IGNORECASE),
                re.compile(r"LogConfig", re.IGNORECASE),
                re.compile(r"LogDerivedDataCache", re.IGNORECASE),
                re.compile(r"LogMemory", re.IGNORECASE),
                re.compile(r"LogGameFeatures", re.IGNORECASE)
            ]

            for line in test_output:
                if "--- Starting subtest:" in line:
                    filtered_lines.append(line)
                    continue
                if not line.startswith('['):
                    continue
                if any(pattern.search(line) for pattern in blocklist_patterns):
                    continue
                if any(pattern.search(line) for pattern in test_filter_patterns):
                    filtered_lines.append(line)
            
            with open(TEST_OUTPUT_FILE, "a") as f: # Append to test output file
                f.writelines(filtered_lines)

            for line in filtered_lines:
                print(line, end='')
            print("--- End Test Results (Filtered) ---")

            if "Automation Test Failed" in process.stdout or "LogAutomationController: Error:" in process.stdout:
                all_tests_passed = False

        except Exception as e:
            print(f"An error occurred during tests: {e}")
            all_tests_passed = False
            
    return all_tests_passed

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run Unreal Engine build and automation tests for SimpleGameplayAbilitySystem.")
    parser.add_argument(
        "tests",
        nargs="*",
        help="Specify which test categories to run (e.g., attributes). "
             "Available categories: " + ", ".join(TEST_CATEGORIES.keys()) + ". "
             "If no arguments are provided, all tests will be run."
    )
    args = parser.parse_args()

    tests_to_run = []
    if args.tests:
        for test_arg in args.tests:
            if test_arg.lower() in TEST_CATEGORIES:
                tests_to_run.extend(TEST_CATEGORIES[test_arg.lower()])
            elif test_arg.startswith("GameTests."): # Allow full automation paths
                tests_to_run.append(test_arg)
            else:
                print(f"Warning: Unknown test category '{test_arg}'. Skipping.")
    else:
        print("No test arguments provided, running all tests. The available test categories are:")
        for category in TEST_CATEGORIES.keys():
            print(f"- {category}")
        for category in TEST_CATEGORIES.values():
            tests_to_run.extend(category)
        # Remove duplicates while preserving order
        tests_to_run = list(dict.fromkeys(tests_to_run))

    if run_build():
        if tests_to_run:
            if run_tests(tests_to_run):
                print("All selected tests passed successfully.")
            else:
                print("Some selected tests failed.")
        else:
            print("No valid test categories specified to run.")
    else:
        print("Build failed, not running tests.")
