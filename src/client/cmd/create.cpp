/*
 * Copyright (C) 2017 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Alberto Aguirre <alberto.aguirre@canonical.com>
 *
 */

#include "create.h"

#include "animated_spinner.h"
#include <multipass/cli/argparser.h>

#include <yaml-cpp/yaml.h>

#include <QTimeZone>

#include <unordered_map>

namespace mp = multipass;
namespace cmd = multipass::cmd;
using RpcMethod = mp::Rpc::Stub;

mp::ReturnCode cmd::Create::run(mp::ArgParser* parser)
{
    auto ret = parse_args(parser);
    if (ret != ParseCode::Ok)
    {
        return parser->returnCodeFrom(ret);
    }

    request.set_time_zone(QTimeZone::systemTimeZoneId().toStdString());

    mp::AnimatedSpinner spinner{cout};
    auto on_success = [this, &spinner](mp::CreateReply& reply) {
        spinner.stop();
        cout << "Launched: " << reply.vm_instance_name() << "\n";
        return ReturnCode::Ok;
    };

    auto on_failure = [this, &spinner](grpc::Status& status) {
        spinner.stop();
        cerr << "failed to launch: " << status.error_message() << std::endl;

        CreateError create_error;
        create_error.ParseFromString(status.error_details());

        for (const auto& error : create_error.error_codes())
        {
            if (error == CreateError::INVALID_DISK_SIZE)
            {
                cerr << "Invalid disk size value supplied: " << request.disk_space() << "\n";
            }
            else if (error == CreateError::INVALID_MEM_SIZE)
            {
                cerr << "Invalid memory size value supplied: " << request.mem_size() << "\n";
            }
            else if (error == CreateError::INVALID_HOSTNAME)
            {
                cerr << "Invalid instance name supplied: " << request.instance_name() << "\n";
            }
        }

        return ReturnCode::CommandFail;
    };

    auto streaming_callback = [this, &spinner](mp::CreateReply& reply) {
        std::unordered_map<int, std::string> download_messages{
            {DownloadProgress_DownloadTypes_IMAGE, "Retrieving image: "},
            {DownloadProgress_DownloadTypes_KERNEL, "Retrieving kernel image: "},
            {DownloadProgress_DownloadTypes_INITRD, "Retrieving initrd image: "}};

        if (reply.create_oneof_case() == mp::CreateReply::CreateOneofCase::kDownloadProgress)
        {
            auto& download_message = download_messages[reply.download_progress().type()];
            if (reply.download_progress().percent_complete() != "-1")
            {
                spinner.stop();
                cout << "\r";
                cout << download_message << reply.download_progress().percent_complete() << "%" << std::flush;
            }
            else
            {
                spinner.start(download_message);
            }
        }
        else if (reply.create_oneof_case() == mp::CreateReply::CreateOneofCase::kCreateMessage)
        {
            spinner.stop();
            spinner.start(reply.create_message());
        }
    };

    return dispatch(&RpcMethod::create, request, on_success, on_failure, streaming_callback);
}

std::string cmd::Create::name() const
{
    return "launch";
}

QString cmd::Create::short_help() const
{
    return QStringLiteral("Create and start an Ubuntu instance");
}

QString cmd::Create::description() const
{
    return QStringLiteral("Create and start a new instance.");
}

mp::ParseCode cmd::Create::parse_args(mp::ArgParser* parser)
{
    parser->addPositionalArgument("image", "Ubuntu image to start", "[<remote:>]<image>");
    QCommandLineOption cpusOption({"c", "cpus"}, "Number of CPUs to allocate", "cpus", "1");
    QCommandLineOption diskOption({"d", "disk"}, "Disk space to allocate in bytes, or with K, M, G suffix", "disk",
                                  "default");
    QCommandLineOption memOption({"m", "mem"}, "Amount of memory to allocate in bytes, or with K, M, G suffix", "mem",
                                 "1024"); // In MB's
    QCommandLineOption nameOption({"n", "name"}, "Name for the instance", "name");
    QCommandLineOption cloudInitOption("cloud-init", "Path to a user-data cloud-init configuration", "file");
    parser->addOptions({cpusOption, diskOption, memOption, nameOption, cloudInitOption});

    auto status = parser->commandParse(this);

    if (status != ParseCode::Ok)
    {
        return status;
    }

    if (parser->positionalArguments().count() > 1)
    {
        cerr << "Too many arguments supplied" << std::endl;
        return ParseCode::CommandLineError;
    }

    if (!parser->positionalArguments().isEmpty())
    {
        auto remote_image_name = parser->positionalArguments().first();
        auto colon_count = remote_image_name.count(':');

        if (colon_count > 1)
        {
            cerr << "Invalid remote and source image name supplied" << std::endl;
            return ParseCode::CommandLineError;
        }
        else if (colon_count == 1)
        {
            request.set_remote_name(remote_image_name.section(':', 0, 0).toStdString());
            request.set_image(remote_image_name.section(':', 1).toStdString());
        }
        else
        {
            request.set_image(remote_image_name.toStdString());
        }
    }

    if (parser->isSet(nameOption))
    {
        request.set_instance_name(parser->value(nameOption).toStdString());
    }

    if (parser->isSet(cpusOption))
    {
        request.set_num_cores(parser->value(cpusOption).toInt());
    }

    if (parser->isSet(memOption))
    {
        request.set_mem_size(parser->value(memOption).toStdString());
    }

    if (parser->isSet(diskOption))
    {
        request.set_disk_space(parser->value(diskOption).toStdString());
    }

    if (parser->isSet(cloudInitOption))
    {
        try
        {
            auto node = YAML::LoadFile(parser->value(cloudInitOption).toStdString());
            request.set_cloud_init_user_data(YAML::Dump(node));
        }
        catch (const std::exception& e)
        {
            cerr << "error loading cloud-init config: " << e.what() << "\n";
            return ParseCode::CommandLineError;
        }
    }

    return status;
}
