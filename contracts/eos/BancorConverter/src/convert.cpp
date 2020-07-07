void BancorConverter::convert(name from, asset quantity, string memo, name code) {
    settings settings_table(get_self(), get_self().value);
    const auto& settings = settings_table.get();
    check(from == settings.network, "converter can only receive from network contract");

    const memo_structure memo_object = parse_memo(memo);
    check(memo_object.path.size() > 1, "invalid memo format");
    check(memo_object.converters[0].account == get_self(), "wrong converter");

    const symbol_code from_path_currency = quantity.symbol.code();
    const symbol_code to_path_currency = symbol_code(memo_object.path[1].c_str());

    const symbol_code converter_currency_code = symbol_code(memo_object.converters[0].sym);
    converters converters_table(get_self(), get_self().value);
    const auto& converter = converters_table.get(converter_currency_code.raw(), "converter does not exist");

    check(from_path_currency != to_path_currency, "cannot convert equivalent currencies");
    check(
        (quantity.symbol == converter.currency && code == settings.multi_token) ||
        code == get_reserve(from_path_currency, converter.currency.code()).contract
        , "unknown 'from' contract");

    const extended_asset from_token = extended_asset(quantity, code);
    extended_symbol to_token;
    if (to_path_currency == converter.currency.code()) {
        check(memo_object.path.size() == 2, "smart token must be final currency");
        to_token = extended_symbol(converter.currency, settings.multi_token);
    }
    else {
        const reserve_t& r = get_reserve(to_path_currency, converter.currency.code());
        to_token = extended_symbol(r.balance.symbol, r.contract);
    }

    auto [to_return, fee] = calculate_return(from_token, to_token, memo, converter, settings.multi_token);
    apply_conversion(memo_object, from_token, extended_asset(to_return, to_token.get_contract()), converter.currency);

    EMIT_CONVERSION_EVENT(
        converter.currency.code(), memo, from_token.contract, from_path_currency, to_token.get_contract(), to_path_currency,
        asset_to_double(quantity),
        asset_to_double(to_return),
        fee
    );

    // MIGRATE DATA to V2
    migrate_converters_v2( converter_currency_code );
}

std::tuple<asset, double> BancorConverter::calculate_return(extended_asset from_token, extended_symbol to_token, string memo, const converter_t& converter, name multi_token) {
    const symbol from_symbol = from_token.quantity.symbol;
    const symbol to_symbol = to_token.get_symbol();

    const bool incoming_smart_token = from_symbol == converter.currency;
    const bool outgoing_smart_token = to_symbol == converter.currency;

    const asset supply = get_supply(multi_token, converter.currency.code());
    double current_smart_supply = supply.amount / pow(10, converter.currency.precision());

    double current_from_balance, current_to_balance;
    reserve_t input_reserve, to_reserve;
    if (!incoming_smart_token) {
        input_reserve = get_reserve(from_symbol.code(), converter.currency.code());
        current_from_balance = asset_to_double(input_reserve.balance);
    }
    if (!outgoing_smart_token) {
        to_reserve = get_reserve(to_symbol.code(), converter.currency.code());
        current_to_balance = asset_to_double(to_reserve.balance);
    }
    const bool quick_conversion = !incoming_smart_token && !outgoing_smart_token && input_reserve.ratio == to_reserve.ratio;

    double from_amount = asset_to_double(from_token.quantity);
    double to_amount;
    if (quick_conversion) { // Reserve --> Reserve
        to_amount = quick_convert(current_from_balance, from_amount, current_to_balance);
    }
    else {
        if (!incoming_smart_token) { // Reserve --> Smart
            to_amount = calculate_purchase_return(current_from_balance, from_amount, current_smart_supply, input_reserve.ratio);
            current_smart_supply += to_amount;
            from_amount = to_amount;
        }
        if (!outgoing_smart_token) { // Smart --> Reserve
            to_amount = calculate_sale_return(current_to_balance, from_amount, current_smart_supply, to_reserve.ratio);
        }
    }

    const uint8_t magnitude = incoming_smart_token || outgoing_smart_token ? 1 : 2;
    const double fee = calculate_fee(to_amount, converter.fee, magnitude);
    to_amount -= fee;

    return std::tuple(
        double_to_asset(to_amount, to_symbol),
        to_fixed(fee, to_symbol.precision())
    );
}

void BancorConverter::apply_conversion(memo_structure memo_object, extended_asset from_token, extended_asset to_return, symbol converter_currency) {
    path new_path = memo_object.path;
    new_path.erase(new_path.begin(), new_path.begin() + 2);
    memo_object.path = new_path;

    auto new_memo = build_memo(memo_object);

    if (from_token.quantity.symbol == converter_currency) {
        action(
            permission_level{ get_self(), "active"_n },
            from_token.contract, "retire"_n,
            make_tuple(from_token.quantity, string("destroy on conversion"))
        ).send();
        mod_reserve_balance(converter_currency, -to_return.quantity, -from_token.quantity.amount);
    }
    else if (to_return.quantity.symbol == converter_currency) {
        mod_reserve_balance(converter_currency, from_token.quantity, to_return.quantity.amount);
        action(
            permission_level{ get_self(), "active"_n },
            to_return.contract, "issue"_n,
            make_tuple(get_self(), to_return.quantity, new_memo)
        ).send();
    }
    else {
        mod_reserve_balance(converter_currency, from_token.quantity);
        mod_reserve_balance(converter_currency, -to_return.quantity);
    }

    check(to_return.quantity.amount > 0, "below min return");

    settings settings_table(get_self(), get_self().value);
    const auto& st = settings_table.get();
    action(
        permission_level{ get_self(), "active"_n },
        to_return.contract, "transfer"_n,
        make_tuple(get_self(), st.network, to_return.quantity, new_memo)
    ).send();

    // MIGRATE DATA to V2
    migrate_converters_v2( converter_currency.code() );
}