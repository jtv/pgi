#pragma once
#include <iostream>
#include <fstream>
#include "string_utls.hpp"
#include "datetime.hpp"
#include "map_utls.hpp"
#include <chrono>
#include <iomanip>

namespace pgi {

class DatabaseWorker
{
public:
    DatabaseWorker(const std::string& configuration_file, const std::string& output_file = "")
    {
        db_config_ = YAML::LoadFile(configuration_file);
        YAML::Node connection_config = db_config_["connection"];
        connect(connection_config);

        YAML::Node tables = db_config_["tables"];
        explore_tables(tables);

        if (!output_file.empty())
            drop_yaml(output_file);
    }

    pqxx::result select(const std::string& table_name,
        const std::vector<std::string> fields = std::vector<std::string>(),
        const std::string& condition = "",
        const int& limit = 100)
    {
        explore_if_unknown(table_name);
        std::stringstream ss;
        if (fields.size() == 0)
            ss << "SELECT *";
        else
        {
            ss << "SELECT " << fields[0];
            for (size_t i = 1; i < fields.size(); i++)
                ss << ", " << fields[i];
        }

        ss << " FROM " << table_name;
        if (!condition.empty())
            ss << " WHERE " << condition;
        ss << " LIMIT " << limit;
        pqxx::result r = execute(ss.str());
        return r;
    }

    pqxx::result select_all_columns(const std::string& table_name, const std::string& condition = "")
    {
        return select(table_name, std::vector<std::string>(), condition);
    }

    void print(pqxx::result r)
    {
        bool header_added = false;
        std::stringstream ss;
        int const num_rows = std::size(r);
        for (int rownum = 0; rownum < num_rows; ++rownum)
        {
            pqxx::row const row = r[rownum];
            ss << print_row(row, !header_added);
            if (!header_added)
                header_added = true;
            ss << "\n";
        }
        std::cout << ss.str();
    }

    std::string print_row(const pqxx::row& row, bool header = false)
    {
        const char fill = ' ';
        const char separator[3] = " |";
        std::stringstream ss;
        std::stringstream headerss;
        int const num_cols = std::size(row);
        for (int colnum = 0; colnum < num_cols; ++colnum)
        {
            pqxx::field const field = row[colnum];
            std::string column_name = field.name();
            std::string column_type = get_typname_from_oid(field.type());
            YAML::Node field_length_mapping = db_config_["field_length_mapping"];
            int field_width;
            if (field_length_mapping[column_type])
                field_width = field_length_mapping[column_type].as<int>();
            else
                field_width = 10;
            field_width = std::max<int>(column_name.length(), field_width);
            ss << std::left << std::setw(field_width) << std::setfill(fill) << utl::truncate(field.c_str(), field_width)
               << separator;
            if (header)
                headerss << std::left << std::setw(field_width) << std::setfill(fill)
                         << utl::truncate(column_name, field_width) << separator;
        }
        if (header)
        {
            headerss << "\n" << ss.str();
            return headerss.str();
        }
        else
        {
            return ss.str();
        }
    }

    void print(const std::string& table_name) { return print(select_all_columns(table_name)); }


    /// Inserts a row in a defined table from a full set of values.
    /// The order of values must be the same as in the table definition.
    template <typename... Args>
    void insert(const std::string& table_name, Args... values)
    {
        explore_if_unknown(table_name);
        std::stringstream ss;
        ss << insert_statement_first_part(table_name);

        ((ss << values << ", "), ...);
        ss.seekp(-2, ss.cur);
        ss << ')';
        execute(ss.str());
    }

    /// Inserts a row in a defined table from a multiple std::map<std::string, T>.
    template <typename... Args>
    void insert_from_maps(const std::string& table_name, Args... maps)
    {
        std::map<std::string, std::string> merged_maps = utl::merge_maps(maps...);
        std::stringstream columns, values, ss;
        columns << "(";
        values << "(";
        for (auto const& [key, val] : merged_maps)
        {
            columns << key << ", ";
            values << val << ", ";
        }
        columns.seekp(-2, columns.cur);
        columns << ")";
        values.seekp(-2, values.cur);
        values << ")";

        ss << "INSERT INTO " << table_name << " " << columns.str() << "VALUES " << values.str();
        explore_if_unknown(table_name);
        execute(ss.str());
    }


    // Specialization for vectors
    template <typename T>
    void insert(const std::string& table_name, const std::vector<T>& vector)
    {
        explore_if_unknown(table_name);
        std::stringstream ss;
        ss << insert_statement_first_part(table_name);

        for (T element : vector)
            ss << element << ", ";
        ss.seekp(-2, ss.cur);
        ss << ')';
        execute(ss.str());
    }

    // Specialization for timed vectors
    template <typename T>
    void insert(const std::string& table_name, time_point_t tp, const std::vector<T>& vector)
    {
        explore_if_unknown(table_name);
        std::stringstream ss;
        ss << insert_statement_first_part(table_name);
        ss << "'" << utl::ISO_8601(tp) << "', ";
        for (T element : vector)
            ss << element << ", ";
        ss.seekp(-2, ss.cur);
        ss << ')';
        execute(ss.str());
    }

    void clear(const std::string& table_name)
    {
        std::stringstream ss;
        ss << "TRUNCATE " << table_name << " CASCADE";
        execute(ss.str());
    }

private:
    void connect(YAML::Node connection_config)
    {
        try
        {
            std::stringstream ss;
            for (YAML::const_iterator it = connection_config.begin(); it != connection_config.end(); ++it)
                ss << it->first.as<std::string>() << "=" << it->second.as<std::string>() << " ";

            std::string connection_string = ss.str();

            std::shared_ptr<pqxx::connection> buff(new pqxx::connection(connection_string));
            c_ = buff;

        } catch (const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
    };

    void explore_tables(YAML::Node tables)
    {
        try
        {
            for (std::size_t i = 0; i < tables.size(); i++)
                get_column_details(tables[i].as<std::string>());
        } catch (const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
    }

    void get_column_details(const std::string& table_name)
    {
        try
        {
            std::stringstream ss(table_name);
            std::string segment;
            std::vector<std::string> seglist;

            std::getline(ss, segment, '.');
            db_config_["tables_details"][table_name]["schema"] = segment;
            std::getline(ss, segment, '.');
            db_config_["tables_details"][table_name]["table"] = segment;


            pqxx::result r = execute(utl::string_format("SELECT * FROM %s LIMIT 0", table_name));

            for (size_t i = 0; i < size_t(r.columns()); i++)
            {
                db_config_["tables_details"][table_name]["columns"][std::string(r.column_name(i))] =
                    get_typname_from_oid(r.column_type(i));
            }

            r = execute(utl::string_format(
                "SELECT c.column_name, c.data_type "
                "FROM information_schema.table_constraints tc "
                "JOIN information_schema.constraint_column_usage AS ccu USING (constraint_schema, constraint_name) "
                "JOIN information_schema.columns AS c ON c.table_schema = '%s' "
                "  AND tc.table_name = '%s' AND ccu.column_name = c.column_name "
                "WHERE constraint_type = 'PRIMARY KEY';",
                db_config_["tables_details"][table_name]["schema"].as<std::string>(),
                db_config_["tables_details"][table_name]["table"].as<std::string>()));
            if (!r.empty())
                db_config_["tables_details"][table_name]["primary_key"] = r[0][0].as<std::string>();
            else
                db_config_["tables_details"][table_name]["primary_key"] = "_none_";

        } catch (const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
    }

    std::string get_typname_from_oid(int oid)
    {
        pqxx::row row;
        row = execute1(utl::string_format("SELECT t.typname FROM pg_type t WHERE t.oid = %d", oid));
        return row[0].as<std::string>();
    }


    pqxx::result execute(const std::string& statement)
    {
        pqxx::result r;
        try
        {
            pqxx::work w(*c_);
            r = w.exec(statement);
            w.commit();
        } catch (const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
        return r;
    }

    pqxx::row execute1(const std::string& statement)
    {
        pqxx::row r;
        try
        {
            pqxx::work w(*c_);
            r = w.exec1(statement);
            w.commit();
        } catch (const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
        return r;
    }

    void drop_yaml(const std::string& output_file)
    {
        try
        {
            std::ofstream fout;
            fout.open(output_file);
            fout << db_config_ << "\n";
            fout.close();
        } catch (const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
    }

    std::string insert_statement_first_part(const std::string& table_name)
    {
        std::stringstream ss;
        ss << "INSERT INTO " << table_name << "(";
        for (YAML::const_iterator it = db_config_["tables_details"][table_name]["columns"].begin();
             it != db_config_["tables_details"][table_name]["columns"].end(); ++it)
        {
            // Include column only if it is not a primary key ewcept if it is of type timezone
            if ((it->first.as<std::string>() !=
                    db_config_["tables_details"][table_name]["primary_key"].as<std::string>()) ||
                (it->second.as<std::string>().find("timestamp") != std::string::npos))
            {
                ss << it->first.as<std::string>() << ", ";
            }
        }

        ss.seekp(-2, ss.cur);
        ss << ") VALUES(";
        return ss.str();
    }

    void explore_if_unknown(const std::string table_name)
    {
        if (db_config_["tables_details"][table_name])
            return;
        db_config_["tables"].push_back(table_name);
        explore_tables(db_config_["tables"]);
    }

    std::shared_ptr<pqxx::connection> c_;

protected:
    YAML::Node db_config_;
};

}  // namespace pgi