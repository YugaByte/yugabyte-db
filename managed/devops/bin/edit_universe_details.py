#!/usr/bin/env python
import json
import subprocess
import os
import sys
import tempfile


def msg_exit(msg):
    print(msg)
    sys.exit(0)


def run_psql(psql_cmd):
    psql_path = "docker exec postgres psql".split(" ") if docker_based else ["psql"]
    return str(subprocess.check_output(
        psql_path +
        ['-U', 'postgres', '-d', 'yugaware', '-h', 'localhost', '-t', '-c', psql_cmd]).decode('utf-8'))


if os.path.exists("/.dockerenv"):
    msg_exit("It appears that this script is being run from within the yugaware docker container." +
             " Please copy it to the docker host and run it from the docker host.")

docker_based = (0 == os.system("docker ps -a | grep yugaware > /dev/null"))

if len(sys.argv) != 2:
    msg_exit("Usage: {} <uuid>".format(sys.argv[0]))

univ_uuid = sys.argv[1]
json_text = run_psql("select universe_details_json from universe " +
                     "where universe.universe_uuid='{}';".format(univ_uuid))
json_parsed = json.loads(json_text)
json_pretty = json.dumps(json_parsed, indent=4, sort_keys=True)


(_, edit_file_path) = tempfile.mkstemp(prefix="edit")
with open(edit_file_path, "w") as edit_file:
    edit_file.write(json_pretty)

(_, orig_file_path) = tempfile.mkstemp(prefix="orig")
with open(orig_file_path, "w") as orig_file:
    orig_file.write(json_pretty)

os.system("${EDITOR-vim} \"" + edit_file_path + "\"")
ret_code = os.system("diff {} {}".format(orig_file_path, edit_file_path))
if 0 == ret_code:
    msg_exit("No changes made to universe json")

input_method = input if sys.version_info.major == 3 else raw_input

confirm = input_method("Confirm the changes above by typing yes: ")
if confirm != 'yes':
    msg_exit("No changes made to database")

with open(edit_file_path, "r") as edit_file:
    new_json_parsed = json.loads(edit_file.read())

print("Updating universe json")
print(run_psql(("update universe set universe_details_json='{}' " +
               "where universe.universe_uuid='{}';").format(
                    json.dumps(new_json_parsed),
                    univ_uuid)))
