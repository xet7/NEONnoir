#include <fstream>
#include <filesystem>
#include <format>
#include "packfile.h"

namespace fs = std::filesystem;

namespace NEONnoir
{
    uint16_t get_script_offset(std::string const& script_name, assembler_result const& result)
    {
        if (result.scripts_meta.count(script_name) == 0)
        {
            throw packer_error(std::format("Reference to non-existing script '{}'", script_name));
        }

        return result.scripts_meta.at(script_name).start;
    }

    uint16_t get_flag_id(std::string const& flag, assembler_result const& result)
    {
        if (result.flags.count(flag) == 0)
        {
            throw packer_error(std::format("Reference to non-existing flag '{}'", flag));
        }

        return result.flags.at(flag);
    }

    neon_packfile generate_packfile(std::shared_ptr<game_data> const& data, assembler_result const& result)
    {
        auto pak = neon_packfile{};

        for (auto& [k, v] : result.strings)
        {
            pak.string_table.push_back(v.value);
        }

        for (auto const& location : data->locations)
        {
            auto loc = neon_location{};
            loc.name_id = static_cast<uint16_t>(pak.string_table.size());
            pak.string_table.push_back(location.name);

            loc.first_bg_id = static_cast<uint16_t>(pak.string_table.size());
            loc.last_bg_id = loc.first_bg_id + static_cast<uint16_t>(location.backgrounds.size()) - 1;
            for (auto const& background : location.backgrounds)
            {
                auto path = std::filesystem::path{ background };
                
                pak.string_table.push_back(path.stem().replace_extension("iff").string());
            }

            loc.first_scene_id = static_cast<uint16_t>(pak.scenes.size());
            loc.last_scene_id = loc.first_scene_id + static_cast<uint16_t>(location.scenes.size()) - 1;

            for (auto const& scene : location.scenes)
            {
                auto s = neon_scene{};
                s.name_id = static_cast<uint16_t>(pak.string_table.size());
                pak.string_table.push_back(scene.name);

                s.background_id = scene.image_id;

                s.first_region_id = static_cast<uint16_t>(pak.regions.size());
                s.last_region_id = s.first_region_id + static_cast<uint16_t>(scene.regions.size()) - 1;

                for (auto const& region : scene.regions)
                {
                    auto r = neon_region{};
                    r.x1 = region.x;
                    r.y1 = region.y;
                    r.x2 = region.x + region.width;
                    r.y2 = region.y + region.height;
                    r.pointer_id = region.pointer_id;

                    r.description_id = static_cast<uint16_t>(pak.string_table.size());
                    pak.string_table.push_back(region.description);

                    if (region.script != "")
                    {
                        if (result.scripts_meta.count(region.script) == 0)
                        {
                            throw packer_error(std::format("Region '{}/{}/{}' references non-existing script '{}'", location.name, scene.name, region.description, region.script));
                        }
                        else
                        {
                            r.script_offset = result.scripts_meta.at(region.script).start;
                        }
                    }
                    else
                    {
                        r.script_offset = 0xFFFF;
                    }

                    pak.regions.push_back(r);
                }

                pak.scenes.push_back(s);
            }

            pak.locations.push_back(loc);
        }

        for (auto const& dialogue : data->dialogues)
        {
            auto d = neon_dialogue{};
            d.speaker_image = dialogue.image_id;
            
            d.speaker_name = static_cast<uint16_t>(pak.string_table.size());
            pak.string_table.push_back(dialogue.speaker);

            d.first_page_id = static_cast<uint16_t>(pak.pages.size());
            auto page_count = 0;
            for (auto const& page : dialogue.pages)
            {
                auto p = neon_page{};
                p.text_id = static_cast<uint16_t>(pak.string_table.size());
                pak.string_table.push_back(page.text);

                p.page_id = page.next_page_id;
                if (p.page_id != 0xFFFF) p.page_id += d.first_page_id;

                p.first_choice_id = static_cast<uint16_t>(pak.choices.size());
                auto choice_count = 0;
                for (auto const& choice : page.choices)
                {
                    auto c = neon_choice{};
                    c.text_id = static_cast<uint16_t>(pak.string_table.size());
                    pak.string_table.push_back(choice.text);

                    c.page_id = choice.next_page_id;
                    if (c.page_id != 0xFFFF) c.page_id += d.first_page_id;

                    c.script_offset = !choice.has_script
                        ? 0xFFFF
                        : get_script_offset(choice.script, result);

                    c.flag = !choice.has_flag
                        ? 0xFFFF
                        : get_flag_id(choice.flag, result);

                    c.enabled = choice.enabled;

                    pak.choices.push_back(c);

                    choice_count++;
                }

                p.choice_count = choice_count;
                pak.pages.push_back(p);

                page_count++;
            }

            d.page_count = page_count;
            pak.dialogues.push_back(d);
        }

        return pak;
    }

    void write(std::ofstream& stream, uint16_t value)
    {
        auto data = reinterpret_cast<char*>(&value);
        stream.write(&data[1], 1);
        stream.write(&data[0], 1);
    }

    void write(std::ofstream& stream, uint32_t value)
    {
        auto data = reinterpret_cast<char*>(&value);
        stream.write(&data[3], 1);
        stream.write(&data[2], 1);
        stream.write(&data[1], 1);
        stream.write(&data[0], 1);
    }

    void serialize_to_neon_pak(fs::path file_path, std::shared_ptr<game_data> const& data, assembler_result const& result)
    {
        auto pak = generate_packfile(data, result);

        if (!file_path.has_extension())
        {
            file_path = file_path.replace_extension("neon");
        }

        auto neonpack = std::ofstream{ file_path, std::ios::binary | std::ios::trunc };

        // Write the header
        neonpack.write((char*)&pak.header.magic, 4);
        write(neonpack, pak.header.major_version);
        write(neonpack, pak.header.minor_version);

        // Write all locations
        neonpack.write(locations_header, 4);
        write(neonpack, static_cast<uint32_t>(pak.locations.size()));
        for (auto const& location: pak.locations)
        {
            write(neonpack, location.name_id);
            write(neonpack, location.first_bg_id);
            write(neonpack, location.last_bg_id);
            write(neonpack, location.first_scene_id);
            write(neonpack, location.last_scene_id);
        }

        // Write all scenes
        neonpack.write(scenes_header, 4);
        write(neonpack, static_cast<uint32_t>(pak.scenes.size()));
        for (auto const& scene : pak.scenes)
        {
            write(neonpack, scene.name_id);
            write(neonpack, scene.background_id);
            write(neonpack, scene.first_region_id);
            write(neonpack, scene.last_region_id);
        }

        // Write all regions
        neonpack.write(regions_header, 4);
        write(neonpack, static_cast<uint32_t>(pak.regions.size()));
        for (auto const& region : pak.regions)
        {
            write(neonpack, region.x1);
            write(neonpack, region.y1);
            write(neonpack, region.x2);
            write(neonpack, region.y2);
            write(neonpack, region.pointer_id);
            write(neonpack, region.description_id);
            write(neonpack, region.script_offset);
        }

        // Dialogues
        neonpack.write(dialogues_header, 4);
        write(neonpack, static_cast<uint32_t>(pak.dialogues.size()));

        for (auto const& dialogue : pak.dialogues)
        {
            write(neonpack, dialogue.first_page_id);
            write(neonpack, dialogue.page_count);
            write(neonpack, dialogue.speaker_name);
            write(neonpack, dialogue.speaker_image);
        }

        // Pages
        neonpack.write(pages_header, 4);
        write(neonpack, static_cast<uint32_t>(pak.pages.size()));

        for (auto const& page : pak.pages)
        {
            write(neonpack, page.text_id);
            write(neonpack, page.page_id);
            write(neonpack, page.first_choice_id);
            write(neonpack, page.choice_count);
            neonpack.write(&page.enabled, 1);
            neonpack.write(&page.padding, 1);
        }

        // Choices
        neonpack.write(choices_header, 4);
        write(neonpack, static_cast<uint32_t>(pak.choices.size()));

        for (auto const& choice : pak.choices)
        {
            write(neonpack, choice.text_id);
            write(neonpack, choice.flag);
            write(neonpack, choice.page_id);
            write(neonpack, choice.script_offset);
            neonpack.write(&choice.enabled, 1);
            neonpack.write(&choice.padding, 1);
        }

        // Write the bytecode "header
        neonpack.write(bytecode_header, 4);
        write(neonpack, static_cast<uint32_t>(result.bytecode.size()));
        for (auto const& code : result.bytecode)
        {
            write(neonpack, static_cast<uint16_t>(code));
        }

        // Write the string table "header"
        neonpack.write(string_header, 4);
        write(neonpack, static_cast<uint32_t>(pak.string_table.size()));

        auto string_size = 0u;
        for (auto const& entry : pak.string_table)
        {
            string_size += static_cast<uint32_t>(entry.size());
            string_size += 4;
        }
        write(neonpack, string_size);

        for (auto const& entry : pak.string_table)
        {
            write(neonpack, static_cast<uint32_t>(entry.size()));
            neonpack.write(entry.data(), entry.size());
        }

        // Add a closing null
        write(neonpack, 0u);

        neonpack.close();
    }
}