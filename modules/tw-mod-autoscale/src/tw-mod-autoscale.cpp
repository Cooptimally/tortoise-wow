#include "Config/Config.h"
#include "Creature.h"
#include "LootMgr.h"
#include "Map.h"
#include "Player.h"
#include "ScriptObjects.h"
#include "Unit.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using DamageSnapshot = std::tuple<std::pair<float, float>, std::pair<float, float>, int32>;

    class ReadMutexGuard
    {
    public:
        explicit ReadMutexGuard(std::shared_mutex& mutex)
            : m_mutex(mutex)
        {
            m_mutex.lock_shared();
        }

        ~ReadMutexGuard() noexcept
        {
            m_mutex.unlock_shared();
        }

        ReadMutexGuard(ReadMutexGuard const&) = delete;
        ReadMutexGuard& operator=(ReadMutexGuard const&) = delete;

    private:
        std::shared_mutex& m_mutex;
    };

    class AutoScaler
    {
    public:
        static AutoScaler& Instance()
        {
            static AutoScaler instance;
            return instance;
        }

        bool IsEnabled() const
        {
            return sConfig.GetBoolDefault("TWAutoScale.Enable", false);
        }
        uint32 GetScalingMaxPlayers(DungeonMap const* dungeonMap) const
        {
            if (!dungeonMap)
                return 1;

            // Turtle/Vanilla dungeon DBC entries can retain historical
            // admission caps such as Deadmines = 10. For autoscaling,
            // ordinary non-raid dungeons belong to the 5-player bucket.
            if (!dungeonMap->IsRaid())
                return 5;

            // Raids retain their real DBC capacity so the 10/20/40-player
            // scaling buckets continue to behave normally.
            return std::max<uint32>(dungeonMap->GetMaxPlayers(), 1);
        }

        void ScaleMap(Map* map)
        {
            if (!IsEnabled())
                return;

            DungeonMap* dungeonMap = dynamic_cast<DungeonMap*>(map);
            if (!dungeonMap)
                return;

            uint32 const playerCount = dungeonMap->GetPlayersCountExceptGMs();
            if (!playerCount)
                return;

            uint32 const maxCount = GetScalingMaxPlayers(dungeonMap);

            auto& lock = dungeonMap->GetObjectLock();
            ReadMutexGuard guard(lock);
            auto& container = const_cast<TypeUnorderedMapContainer<AllMapStoredObjectTypes, ObjectGuid>&>(dungeonMap->GetObjectStore());

            auto pairItr = container.range<Creature>();
            while (pairItr.first != pairItr.second)
            {
                Creature* creature = pairItr.first->second;
                if (creature && !creature->IsInCombat())
                    ScaleCreature(creature, playerCount, maxCount);

                ++pairItr.first;
            }
        }

        void ScaleCreatureForCurrentMap(Creature* creature)
        {
            if (!IsEnabled() || !creature)
                return;

            DungeonMap* dungeonMap = dynamic_cast<DungeonMap*>(creature->GetMap());
            if (!dungeonMap)
                return;

            uint32 const playerCount = dungeonMap->GetPlayersCountExceptGMs();
            if (!playerCount)
                return;

            ScaleCreature(creature, playerCount, GetScalingMaxPlayers(dungeonMap));
        }

        void ScaleCreature(Creature* creature, uint32 playerCount, uint32 maxCount)
        {
            if (!creature || creature->IsDead())
                return;

            if (creature->IsPet() && creature->GetOwner() && creature->GetOwner()->IsPlayer())
                return;

            uint32 const maxPlayers = std::max<uint32>(maxCount, 1);
            float const clampedPlayers = static_cast<float>(std::min<uint32>(std::max<uint32>(playerCount, 1), maxPlayers));
            float healthScaleFactor = clampedPlayers / static_cast<float>(maxPlayers);
            float damageScaleFactor = clampedPlayers / static_cast<float>(maxPlayers);

            ApplyConfiguredClamp(maxPlayers, healthScaleFactor, damageScaleFactor);

            if (maxPlayers <= 5 && IsConfiguredFiveManBoss(creature))
            {
                healthScaleFactor = std::max(
                    healthScaleFactor,
                    sConfig.GetFloatDefault("TWAutoScale.ScalarMin5ManBossHP", 0.70f));
            }

            auto const healthScaleValue = [healthScaleFactor](float value)
            {
                return value * healthScaleFactor;
            };

            auto const damageScaleValue = [damageScaleFactor](float value)
            {
                return value * damageScaleFactor;
            };

            creature->SetMaxHealth(std::max(1u, static_cast<uint32>(healthScaleValue(static_cast<float>(creature->GetCreateHealth())))));

            // In 5-player content, outgoing creature damage is scaled by the
            // UnitScript melee/spell hooks below. Returning here prevents the
            // old physical-stat path from scaling the same attacks a second time.
            if (maxPlayers <= 5)
                return;

            DamageSnapshot& snapshot = GetOrCreateDamageSnapshot(creature);

            creature->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, damageScaleValue(std::get<0>(snapshot).first));
            creature->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, damageScaleValue(std::get<0>(snapshot).second));

            creature->SetBaseWeaponDamage(OFF_ATTACK, MINDAMAGE, damageScaleValue(std::get<0>(snapshot).first));
            creature->SetBaseWeaponDamage(OFF_ATTACK, MAXDAMAGE, damageScaleValue(std::get<0>(snapshot).second));

            creature->SetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE, damageScaleValue(std::get<1>(snapshot).first));
            creature->SetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE, damageScaleValue(std::get<1>(snapshot).second));

            creature->SetInt32Value(UNIT_FIELD_ATTACK_POWER, static_cast<int32>(damageScaleValue(static_cast<float>(std::get<2>(snapshot)))));

            creature->UpdateDamagePhysical(BASE_ATTACK);
            creature->UpdateDamagePhysical(OFF_ATTACK);
            creature->UpdateDamagePhysical(RANGED_ATTACK);
        }

        void ScaleMoneyLoot(Creature* creature)
        {
            if (!IsEnabled() || !creature)
                return;

            DungeonMap* dungeonMap = dynamic_cast<DungeonMap*>(creature->GetMap());
            if (!dungeonMap)
                return;

            uint32 const maxCount = std::max<uint32>(GetScalingMaxPlayers(dungeonMap), 1);
            uint32 const playerCount = dungeonMap->GetPlayersCountExceptGMs();
            uint32 const clampedPlayers = std::min<uint32>(std::max<uint32>(playerCount, 1), maxCount);
            float const goldFactor = static_cast<float>(clampedPlayers) / static_cast<float>(maxCount);

            creature->loot.gold = static_cast<uint32>(static_cast<float>(creature->loot.gold) * goldFactor);
        }

        void ScaleKillXP(Player* player, uint32& amount, Unit* victim)
        {
            if (!IsEnabled() || !player || !victim || !amount)
                return;

            if (!sConfig.GetBoolDefault("TWAutoScale.ScaleXP", true))
                return;

            DungeonMap* dungeonMap = dynamic_cast<DungeonMap*>(player->GetMap());
            if (!dungeonMap)
                return;

            if (victim->GetMap() != player->GetMap())
                return;

            // If the player has formed a real group, use the core's normal
            // Vanilla group-XP calculation instead of our solo replacement.
            if (player->GetGroup())
                return;

            uint32 const playerCount = dungeonMap->GetPlayersCountExceptGMs();
            if (playerCount != 1)
                return;

            uint32 const maxCount = std::max<uint32>(GetScalingMaxPlayers(dungeonMap), 1);
            if (maxCount > 5)
                return;

            float const configuredFactor = sConfig.GetFloatDefault("TWAutoScale.Scalar5ManXP", 0.28f);
            float const xpFactor = std::max(0.0f, std::min(1.0f, configuredFactor));

            amount = static_cast<uint32>(
                static_cast<float>(amount) * xpFactor + 0.5f);
        }

        void ScaleFiveManMeleeDamage(Unit* attacker, uint32& damage) const
        {
            if (!IsEnabled() || !attacker || !damage)
                return;

            Creature* creature = attacker->ToCreature();
            if (!creature)
                return;

            float const factor =
                GetFiveManDamageScaleFactor(creature);

            if (factor == 1.0f)
                return;

            damage = static_cast<uint32>(
                static_cast<float>(damage) * factor + 0.5f);
        }

        void ScaleFiveManSpellDamage(Unit* attacker, int32& damage) const
        {
            if (!IsEnabled() || !attacker || damage <= 0)
                return;

            Creature* creature = attacker->ToCreature();
            if (!creature)
                return;

            float const factor =
                GetFiveManDamageScaleFactor(creature);

            if (factor == 1.0f)
                return;

            damage = static_cast<int32>(
                static_cast<float>(damage) * factor + 0.5f);
        }

    private:
        static std::unordered_set<uint32> const& GetConfiguredFiveManBossIds()
        {
            static std::unordered_set<uint32> const bossIds = []()
            {
                std::unordered_set<uint32> ids;

                std::string raw = sConfig.GetStringDefault(
                    "TWAutoScale.Scalar5ManBossCreatureIds",
                    "11517,11518,11519,11520");

                std::stringstream ss(raw);
                std::string token;

                while (std::getline(ss, token, ','))
                {
                    token.erase(
                        std::remove_if(
                            token.begin(),
                            token.end(),
                            [](unsigned char c) { return std::isspace(c); }),
                        token.end());

                    if (token.empty())
                        continue;

                    char* end = nullptr;
                    unsigned long const value =
                        std::strtoul(token.c_str(), &end, 10);

                    if (end != token.c_str() &&
                        *end == '\0' &&
                        value <= std::numeric_limits<uint32>::max())
                    {
                        ids.insert(static_cast<uint32>(value));
                    }
                }

                return ids;
            }();

            return bossIds;
        }

        bool IsConfiguredFiveManBoss(Creature const* creature) const
        {
            if (!creature)
                return false;

            auto const& bossIds = GetConfiguredFiveManBossIds();

            return bossIds.find(creature->GetEntry()) != bossIds.end();
        }

        float GetFiveManDamageScaleFactor(Creature* creature) const
        {
            if (!creature)
                return 1.0f;

            if (creature->IsPet() &&
                creature->GetOwner() &&
                creature->GetOwner()->IsPlayer())
            {
                return 1.0f;
            }

            DungeonMap* dungeonMap =
                dynamic_cast<DungeonMap*>(creature->GetMap());

            if (!dungeonMap)
                return 1.0f;

            uint32 const maxPlayers = std::max<uint32>(
                GetScalingMaxPlayers(dungeonMap),
                1);

            // V1 final-damage scaling is deliberately limited to
            // ordinary 5-player-or-smaller instances.
            if (maxPlayers > 5)
                return 1.0f;

            uint32 const playerCount =
                dungeonMap->GetPlayersCountExceptGMs();

            if (!playerCount)
                return 1.0f;

            float const clampedPlayers = static_cast<float>(
                std::min<uint32>(
                    std::max<uint32>(playerCount, 1),
                    maxPlayers));

            float const playerFactor =
                clampedPlayers / static_cast<float>(maxPlayers);

            bool const isBoss =
                IsConfiguredFiveManBoss(creature);

            char const* damageKey =
                isBoss
                    ? "TWAutoScale.ScalarMin5ManBossDMG"
                    : "TWAutoScale.ScalarMin5ManDMG";

            float const defaultDamageFloor =
                isBoss ? 0.42f : 0.30f;

            float const configuredFloor = std::max(
                0.0f,
                sConfig.GetFloatDefault(
                    damageKey,
                    defaultDamageFloor));

            return std::max(
                playerFactor,
                configuredFloor);
        }

        DamageSnapshot& GetOrCreateDamageSnapshot(Creature* creature)
        {
            auto result = m_baseDamages.emplace(
                creature->GetEntry(),
                std::make_tuple(
                    std::make_pair(creature->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE), creature->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE)),
                    std::make_pair(creature->GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE), creature->GetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE)),
                    creature->GetInt32Value(UNIT_FIELD_ATTACK_POWER)));

            return result.first->second;
        }

        void ApplyConfiguredClamp(uint32 maxPlayers, float& healthScaleFactor, float& damageScaleFactor) const
        {
            if (maxPlayers <= 5)
            {
                healthScaleFactor = std::max(healthScaleFactor, sConfig.GetFloatDefault("TWAutoScale.ScalarMin5ManHP", 0.6f));
                damageScaleFactor = std::max(damageScaleFactor, sConfig.GetFloatDefault("TWAutoScale.ScalarMin5ManDMG", 0.4f));
            }

            else if (maxPlayers <= 10)
            {
                healthScaleFactor = std::max(healthScaleFactor, sConfig.GetFloatDefault("TWAutoScale.ScalarMin10ManHP", 0.6f));
                damageScaleFactor = std::max(damageScaleFactor, sConfig.GetFloatDefault("TWAutoScale.ScalarMin10ManDMG", 0.4f));
            }

            else if (maxPlayers <= 20)
            {
                healthScaleFactor = std::max(healthScaleFactor, sConfig.GetFloatDefault("TWAutoScale.ScalarMin20ManHP", 0.6f));
                damageScaleFactor = std::max(damageScaleFactor, sConfig.GetFloatDefault("TWAutoScale.ScalarMin20ManDMG", 0.4f));
            }

            else if (maxPlayers <= 40)
            {
                healthScaleFactor = std::max(healthScaleFactor, sConfig.GetFloatDefault("TWAutoScale.ScalarMin40ManHP", 0.6f));
                damageScaleFactor = std::max(damageScaleFactor, sConfig.GetFloatDefault("TWAutoScale.ScalarMin40ManDMG", 0.4f));
            }
        }

        std::unordered_map<uint32, DamageSnapshot> m_baseDamages;
    };

    class AutoScaleMapScript : public AllMapScript
    {
    public:
        AutoScaleMapScript()
            : AllMapScript("tw_mod_autoscale_map")
        {
        }

        void OnPlayerEnterAll(Map* map, Player* /*player*/) override
        {
            AutoScaler::Instance().ScaleMap(map);
        }

        void OnPlayerLeaveAll(Map* map, Player* /*player*/) override
        {
            AutoScaler::Instance().ScaleMap(map);
        }
    };

    class AutoScaleUnitScript : public UnitScript
    {
    public:
        AutoScaleUnitScript()
            : UnitScript(
                "tw_mod_autoscale_unit",
                {
                    UNITHOOK_ON_UNIT_ENTER_COMBAT,
                    UNITHOOK_ON_UNIT_DEATH,
                    UNITHOOK_MODIFY_MELEE_DAMAGE,
                    UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN
                })
        {
        }

        void OnUnitEnterCombat(Unit* unit, Unit* /*victim*/) override
        {
            if (Creature* creature = unit ? unit->ToCreature() : nullptr)
                AutoScaler::Instance().ScaleCreatureForCurrentMap(creature);
        }

        void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
        {
            if (Creature* creature = unit ? unit->ToCreature() : nullptr)
                AutoScaler::Instance().ScaleMoneyLoot(creature);
        }

        void ModifyMeleeDamage(
            Unit* /*target*/,
            Unit* attacker,
            uint32& damage) override
        {
            AutoScaler::Instance().ScaleFiveManMeleeDamage(
                attacker,
                damage);
        }

        void ModifySpellDamageTaken(
            Unit* /*target*/,
            Unit* attacker,
            int32& damage,
            SpellEntry const* /*spellInfo*/) override
        {
            AutoScaler::Instance().ScaleFiveManSpellDamage(
                attacker,
                damage);
        }
    };

    class AutoScalePlayerScript : public PlayerScript
    {
    public:
        AutoScalePlayerScript()
            : PlayerScript("tw_mod_autoscale_player", { PLAYERHOOK_ON_GIVE_EXP })
        {
        }

        void OnGiveXP(Player* player, uint32& amount, Unit* victim) override
        {
            AutoScaler::Instance().ScaleKillXP(player, amount, victim);
        }
    };
}

void Addtw_mod_autoscaleScripts()
{
    new AutoScaleMapScript();
    new AutoScaleUnitScript();
    new AutoScalePlayerScript();
}
